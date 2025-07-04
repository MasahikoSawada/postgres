/*-------------------------------------------------------------------------
 *
 * backend_startup.c
 *	  Backend startup code
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/backend_startup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "common/ip.h"
#include "common/string.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"
#include "storage/proc.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/varlena.h"

/* GUCs */
bool		Trace_connection_negotiation = false;
uint32		log_connections = 0;
char	   *log_connections_string = NULL;

/* Other globals */

/*
 * ConnectionTiming stores timestamps of various points in connection
 * establishment and setup.
 * ready_for_use is initialized to a special value here so we can check if
 * we've already set it before doing so in PostgresMain().
 */
ConnectionTiming conn_timing = {.ready_for_use = TIMESTAMP_MINUS_INFINITY};

static void BackendInitialize(ClientSocket *client_sock, CAC_state cac);
static int	ProcessSSLStartup(Port *port);
static int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);
static void ProcessCancelRequestPacket(Port *port, void *pkt, int pktlen);
static void SendNegotiateProtocolVersion(List *unrecognized_protocol_options);
static void process_startup_packet_die(SIGNAL_ARGS);
static void StartupPacketTimeoutHandler(void);
static bool validate_log_connections_options(List *elemlist, uint32 *flags);

/*
 * Entry point for a new backend process.
 *
 * Initialize the connection, read the startup packet, authenticate the
 * client, and start the main processing loop.
 */
void
BackendMain(const void *startup_data, size_t startup_data_len)
{
	const BackendStartupData *bsdata = startup_data;

	Assert(startup_data_len == sizeof(BackendStartupData));
	Assert(MyClientSocket != NULL);

#ifdef EXEC_BACKEND

	/*
	 * Need to reinitialize the SSL library in the backend, since the context
	 * structures contain function pointers and cannot be passed through the
	 * parameter file.
	 *
	 * If for some reason reload fails (maybe the user installed broken key
	 * files), soldier on without SSL; that's better than all connections
	 * becoming impossible.
	 *
	 * XXX should we do this in all child processes?  For the moment it's
	 * enough to do it in backend children.
	 */
#ifdef USE_SSL
	if (EnableSSL)
	{
		if (secure_initialize(false) == 0)
			LoadedSSL = true;
		else
			ereport(LOG,
					(errmsg("SSL configuration could not be loaded in child process")));
	}
#endif
#endif

	/* Perform additional initialization and collect startup packet */
	BackendInitialize(MyClientSocket, bsdata->canAcceptConnections);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we can use LWLocks or access any shared memory.
	 */
	InitProcess();

	/*
	 * Make sure we aren't in PostmasterContext anymore.  (We can't delete it
	 * just yet, though, because InitPostgres will need the HBA data.)
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	PostgresMain(MyProcPort->database_name, MyProcPort->user_name);
}


/*
 * BackendInitialize -- initialize an interactive (postmaster-child)
 *				backend process, and collect the client's startup packet.
 *
 * returns: nothing.  Will not return at all if there's any failure.
 *
 * Note: this code does not depend on having any access to shared memory.
 * Indeed, our approach to SIGTERM/timeout handling *requires* that
 * shared memory not have been touched yet; see comments within.
 * In the EXEC_BACKEND case, we are physically attached to shared memory
 * but have not yet set up most of our local pointers to shmem structures.
 */
static void
BackendInitialize(ClientSocket *client_sock, CAC_state cac)
{
	int			status;
	int			ret;
	Port	   *port;
	char		remote_host[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	StringInfoData ps_data;
	MemoryContext oldcontext;

	/* Tell fd.c about the long-lived FD associated with the client_sock */
	ReserveExternalFD();

	/*
	 * PreAuthDelay is a debugging aid for investigating problems in the
	 * authentication cycle: it can be set in postgresql.conf to allow time to
	 * attach to the newly-forked backend with a debugger.  (See also
	 * PostAuthDelay, which we allow clients to pass through PGOPTIONS, but it
	 * is not honored until after authentication.)
	 */
	if (PreAuthDelay > 0)
		pg_usleep(PreAuthDelay * 1000000L);

	/* This flag will remain set until InitPostgres finishes authentication */
	ClientAuthInProgress = true;	/* limit visibility of log messages */

	/*
	 * Initialize libpq and enable reporting of ereport errors to the client.
	 * Must do this now because authentication uses libpq to send messages.
	 *
	 * The Port structure and all data structures attached to it are allocated
	 * in TopMemoryContext, so that they survive into PostgresMain execution.
	 * We need not worry about leaking this storage on failure, since we
	 * aren't in the postmaster process anymore.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	port = MyProcPort = pq_init(client_sock);
	MemoryContextSwitchTo(oldcontext);

	whereToSendOutput = DestRemote; /* now safe to ereport to client */

	/* set these to empty in case they are needed before we set them up */
	port->remote_host = "";
	port->remote_port = "";

	/*
	 * We arrange to do _exit(1) if we receive SIGTERM or timeout while trying
	 * to collect the startup packet; while SIGQUIT results in _exit(2).
	 * Otherwise the postmaster cannot shutdown the database FAST or IMMED
	 * cleanly if a buggy client fails to send the packet promptly.
	 *
	 * Exiting with _exit(1) is only possible because we have not yet touched
	 * shared memory; therefore no outside-the-process state needs to get
	 * cleaned up.
	 */
	pqsignal(SIGTERM, process_startup_packet_die);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	InitializeTimeouts();		/* establishes SIGALRM handler */
	sigprocmask(SIG_SETMASK, &StartupBlockSig, NULL);

	/*
	 * Get the remote host name and port for logging and status display.
	 */
	remote_host[0] = '\0';
	remote_port[0] = '\0';
	if ((ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								  remote_host, sizeof(remote_host),
								  remote_port, sizeof(remote_port),
								  (log_hostname ? 0 : NI_NUMERICHOST) | NI_NUMERICSERV)) != 0)
		ereport(WARNING,
				(errmsg_internal("pg_getnameinfo_all() failed: %s",
								 gai_strerror(ret))));

	/*
	 * Save remote_host and remote_port in port structure (after this, they
	 * will appear in log_line_prefix data for log messages).
	 */
	port->remote_host = MemoryContextStrdup(TopMemoryContext, remote_host);
	port->remote_port = MemoryContextStrdup(TopMemoryContext, remote_port);

	/* And now we can log that the connection was received, if enabled */
	if (log_connections & LOG_CONNECTION_RECEIPT)
	{
		if (remote_port[0])
			ereport(LOG,
					(errmsg("connection received: host=%s port=%s",
							remote_host,
							remote_port)));
		else
			ereport(LOG,
					(errmsg("connection received: host=%s",
							remote_host)));
	}

	/* For testing client error handling */
#ifdef USE_INJECTION_POINTS
	INJECTION_POINT("backend-initialize", NULL);
	if (IS_INJECTION_POINT_ATTACHED("backend-initialize-v2-error"))
	{
		/*
		 * This simulates an early error from a pre-v14 server, which used the
		 * version 2 protocol for any errors that occurred before processing
		 * the startup packet.
		 */
		FrontendProtocol = PG_PROTOCOL(2, 0);
		elog(FATAL, "protocol version 2 error triggered");
	}
#endif

	/*
	 * If we did a reverse lookup to name, we might as well save the results
	 * rather than possibly repeating the lookup during authentication.
	 *
	 * Note that we don't want to specify NI_NAMEREQD above, because then we'd
	 * get nothing useful for a client without an rDNS entry.  Therefore, we
	 * must check whether we got a numeric IPv4 or IPv6 address, and not save
	 * it into remote_hostname if so.  (This test is conservative and might
	 * sometimes classify a hostname as numeric, but an error in that
	 * direction is safe; it only results in a possible extra lookup.)
	 */
	if (log_hostname &&
		ret == 0 &&
		strspn(remote_host, "0123456789.") < strlen(remote_host) &&
		strspn(remote_host, "0123456789ABCDEFabcdef:") < strlen(remote_host))
	{
		port->remote_hostname = MemoryContextStrdup(TopMemoryContext, remote_host);
	}

	/*
	 * Ready to begin client interaction.  We will give up and _exit(1) after
	 * a time delay, so that a broken client can't hog a connection
	 * indefinitely.  PreAuthDelay and any DNS interactions above don't count
	 * against the time limit.
	 *
	 * Note: AuthenticationTimeout is applied here while waiting for the
	 * startup packet, and then again in InitPostgres for the duration of any
	 * authentication operations.  So a hostile client could tie up the
	 * process for nearly twice AuthenticationTimeout before we kick him off.
	 *
	 * Note: because PostgresMain will call InitializeTimeouts again, the
	 * registration of STARTUP_PACKET_TIMEOUT will be lost.  This is okay
	 * since we never use it again after this function.
	 */
	RegisterTimeout(STARTUP_PACKET_TIMEOUT, StartupPacketTimeoutHandler);
	enable_timeout_after(STARTUP_PACKET_TIMEOUT, AuthenticationTimeout * 1000);

	/* Handle direct SSL handshake */
	status = ProcessSSLStartup(port);

	/*
	 * Receive the startup packet (which might turn out to be a cancel request
	 * packet).
	 */
	if (status == STATUS_OK)
		status = ProcessStartupPacket(port, false, false);

	/*
	 * If we're going to reject the connection due to database state, say so
	 * now instead of wasting cycles on an authentication exchange. (This also
	 * allows a pg_ping utility to be written.)
	 */
	if (status == STATUS_OK)
	{
		switch (cac)
		{
			case CAC_STARTUP:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is starting up")));
				break;
			case CAC_NOTHOTSTANDBY:
				if (!EnableHotStandby)
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not accepting connections"),
							 errdetail("Hot standby mode is disabled.")));
				else if (reachedConsistency)
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not yet accepting connections"),
							 errdetail("Recovery snapshot is not yet ready for hot standby."),
							 errhint("To enable hot standby, close write transactions with more than %d subtransactions on the primary server.",
									 PGPROC_MAX_CACHED_SUBXIDS)));
				else
					ereport(FATAL,
							(errcode(ERRCODE_CANNOT_CONNECT_NOW),
							 errmsg("the database system is not yet accepting connections"),
							 errdetail("Consistent recovery state has not been yet reached.")));
				break;
			case CAC_SHUTDOWN:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is shutting down")));
				break;
			case CAC_RECOVERY:
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is in recovery mode")));
				break;
			case CAC_TOOMANY:
				ereport(FATAL,
						(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						 errmsg("sorry, too many clients already")));
				break;
			case CAC_OK:
				break;
		}
	}

	/*
	 * Disable the timeout, and prevent SIGTERM again.
	 */
	disable_timeout(STARTUP_PACKET_TIMEOUT, false);
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/*
	 * As a safety check that nothing in startup has yet performed
	 * shared-memory modifications that would need to be undone if we had
	 * exited through SIGTERM or timeout above, check that no on_shmem_exit
	 * handlers have been registered yet.  (This isn't terribly bulletproof,
	 * since someone might misuse an on_proc_exit handler for shmem cleanup,
	 * but it's a cheap and helpful check.  We cannot disallow on_proc_exit
	 * handlers unfortunately, since pq_init() already registered one.)
	 */
	check_on_shmem_exit_lists_are_empty();

	/*
	 * Stop here if it was bad or a cancel packet.  ProcessStartupPacket
	 * already did any appropriate error reporting.
	 */
	if (status != STATUS_OK)
		proc_exit(0);

	/*
	 * Now that we have the user and database name, we can set the process
	 * title for ps.  It's good to do this as early as possible in startup.
	 */
	initStringInfo(&ps_data);
	if (am_walsender)
		appendStringInfo(&ps_data, "%s ", GetBackendTypeDesc(B_WAL_SENDER));
	appendStringInfo(&ps_data, "%s ", port->user_name);
	if (port->database_name[0] != '\0')
		appendStringInfo(&ps_data, "%s ", port->database_name);
	appendStringInfoString(&ps_data, port->remote_host);
	if (port->remote_port[0] != '\0')
		appendStringInfo(&ps_data, "(%s)", port->remote_port);

	init_ps_display(ps_data.data);
	pfree(ps_data.data);

	set_ps_display("initializing");
}

/*
 * Check for a direct SSL connection.
 *
 * This happens before the startup packet so we are careful not to actually
 * read any bytes from the stream if it's not a direct SSL connection.
 */
static int
ProcessSSLStartup(Port *port)
{
	int			firstbyte;

	Assert(!port->ssl_in_use);

	pq_startmsgread();
	firstbyte = pq_peekbyte();
	pq_endmsgread();
	if (firstbyte == EOF)
	{
		/*
		 * Like in ProcessStartupPacket, if we get no data at all, don't
		 * clutter the log with a complaint.
		 */
		return STATUS_ERROR;
	}

	if (firstbyte != 0x16)
	{
		/* Not an SSL handshake message */
		return STATUS_OK;
	}

	/*
	 * First byte indicates standard SSL handshake message
	 *
	 * (It can't be a Postgres startup length because in network byte order
	 * that would be a startup packet hundreds of megabytes long)
	 */

#ifdef USE_SSL
	if (!LoadedSSL || port->laddr.addr.ss_family == AF_UNIX)
	{
		/* SSL not supported */
		goto reject;
	}

	if (secure_open_server(port) == -1)
	{
		/*
		 * we assume secure_open_server() sent an appropriate TLS alert
		 * already
		 */
		goto reject;
	}
	Assert(port->ssl_in_use);

	if (!port->alpn_used)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("received direct SSL connection request without ALPN protocol negotiation extension")));
		goto reject;
	}

	if (Trace_connection_negotiation)
		ereport(LOG,
				(errmsg("direct SSL connection accepted")));
	return STATUS_OK;
#else
	/* SSL not supported by this build */
	goto reject;
#endif

reject:
	if (Trace_connection_negotiation)
		ereport(LOG,
				(errmsg("direct SSL connection rejected")));
	return STATUS_ERROR;
}

/*
 * Read a client's startup packet and do something according to it.
 *
 * Returns STATUS_OK or STATUS_ERROR, or might call ereport(FATAL) and
 * not return at all.
 *
 * (Note that ereport(FATAL) stuff is sent to the client, so only use it
 * if that's what you want.  Return STATUS_ERROR if you don't want to
 * send anything to the client, which would typically be appropriate
 * if we detect a communications failure.)
 *
 * Set ssl_done and/or gss_done when negotiation of an encrypted layer
 * (currently, TLS or GSSAPI) is completed. A successful negotiation of either
 * encryption layer sets both flags, but a rejected negotiation sets only the
 * flag for that layer, since the client may wish to try the other one. We
 * should make no assumption here about the order in which the client may make
 * requests.
 */
static int
ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done)
{
	int32		len;
	char	   *buf;
	ProtocolVersion proto;
	MemoryContext oldcontext;

	pq_startmsgread();

	/*
	 * Grab the first byte of the length word separately, so that we can tell
	 * whether we have no data at all or an incomplete packet.  (This might
	 * sound inefficient, but it's not really, because of buffering in
	 * pqcomm.c.)
	 */
	if (pq_getbytes(&len, 1) == EOF)
	{
		/*
		 * If we get no data at all, don't clutter the log with a complaint;
		 * such cases often occur for legitimate reasons.  An example is that
		 * we might be here after responding to NEGOTIATE_SSL_CODE, and if the
		 * client didn't like our response, it'll probably just drop the
		 * connection.  Service-monitoring software also often just opens and
		 * closes a connection without sending anything.  (So do port
		 * scanners, which may be less benign, but it's not really our job to
		 * notice those.)
		 */
		return STATUS_ERROR;
	}

	if (pq_getbytes(((char *) &len) + 1, 3) == EOF)
	{
		/* Got a partial length word, so bleat about that */
		if (!ssl_done && !gss_done)
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	len = pg_ntoh32(len);
	len -= 4;

	if (len < (int32) sizeof(ProtocolVersion) ||
		len > MAX_STARTUP_PACKET_LENGTH)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * Allocate space to hold the startup packet, plus one extra byte that's
	 * initialized to be zero.  This ensures we will have null termination of
	 * all strings inside the packet.
	 */
	buf = palloc(len + 1);
	buf[len] = '\0';

	if (pq_getbytes(buf, len) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}
	pq_endmsgread();

	/*
	 * The first field is either a protocol version number or a special
	 * request code.
	 */
	port->proto = proto = pg_ntoh32(*((ProtocolVersion *) buf));

	if (proto == CANCEL_REQUEST_CODE)
	{
		ProcessCancelRequestPacket(port, buf, len);
		/* Not really an error, but we don't want to proceed further */
		return STATUS_ERROR;
	}

	if (proto == NEGOTIATE_SSL_CODE && !ssl_done)
	{
		char		SSLok;

#ifdef USE_SSL

		/*
		 * No SSL when disabled or on Unix sockets.
		 *
		 * Also no SSL negotiation if we already have a direct SSL connection
		 */
		if (!LoadedSSL || port->laddr.addr.ss_family == AF_UNIX || port->ssl_in_use)
			SSLok = 'N';
		else
			SSLok = 'S';		/* Support for SSL */
#else
		SSLok = 'N';			/* No support for SSL */
#endif

		if (Trace_connection_negotiation)
		{
			if (SSLok == 'S')
				ereport(LOG,
						(errmsg("SSLRequest accepted")));
			else
				ereport(LOG,
						(errmsg("SSLRequest rejected")));
		}

		while (secure_write(port, &SSLok, 1) != 1)
		{
			if (errno == EINTR)
				continue;		/* if interrupted, just retry */
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("failed to send SSL negotiation response: %m")));
			return STATUS_ERROR;	/* close the connection */
		}

#ifdef USE_SSL
		if (SSLok == 'S' && secure_open_server(port) == -1)
			return STATUS_ERROR;
#endif

		/*
		 * At this point we should have no data already buffered.  If we do,
		 * it was received before we performed the SSL handshake, so it wasn't
		 * encrypted and indeed may have been injected by a man-in-the-middle.
		 * We report this case to the client.
		 */
		if (pq_buffer_remaining_data() > 0)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received unencrypted data after SSL request"),
					 errdetail("This could be either a client-software bug or evidence of an attempted man-in-the-middle attack.")));

		/*
		 * regular startup packet, cancel, etc packet should follow, but not
		 * another SSL negotiation request, and a GSS request should only
		 * follow if SSL was rejected (client may negotiate in either order)
		 */
		return ProcessStartupPacket(port, true, SSLok == 'S');
	}
	else if (proto == NEGOTIATE_GSS_CODE && !gss_done)
	{
		char		GSSok = 'N';

#ifdef ENABLE_GSS
		/* No GSSAPI encryption when on Unix socket */
		if (port->laddr.addr.ss_family != AF_UNIX)
			GSSok = 'G';
#endif

		if (Trace_connection_negotiation)
		{
			if (GSSok == 'G')
				ereport(LOG,
						(errmsg("GSSENCRequest accepted")));
			else
				ereport(LOG,
						(errmsg("GSSENCRequest rejected")));
		}

		while (secure_write(port, &GSSok, 1) != 1)
		{
			if (errno == EINTR)
				continue;
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("failed to send GSSAPI negotiation response: %m")));
			return STATUS_ERROR;	/* close the connection */
		}

#ifdef ENABLE_GSS
		if (GSSok == 'G' && secure_open_gssapi(port) == -1)
			return STATUS_ERROR;
#endif

		/*
		 * At this point we should have no data already buffered.  If we do,
		 * it was received before we performed the GSS handshake, so it wasn't
		 * encrypted and indeed may have been injected by a man-in-the-middle.
		 * We report this case to the client.
		 */
		if (pq_buffer_remaining_data() > 0)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received unencrypted data after GSSAPI encryption request"),
					 errdetail("This could be either a client-software bug or evidence of an attempted man-in-the-middle attack.")));

		/*
		 * regular startup packet, cancel, etc packet should follow, but not
		 * another GSS negotiation request, and an SSL request should only
		 * follow if GSS was rejected (client may negotiate in either order)
		 */
		return ProcessStartupPacket(port, GSSok == 'G', true);
	}

	/* Could add additional special packet types here */

	/*
	 * Set FrontendProtocol now so that ereport() knows what format to send if
	 * we fail during startup. We use the protocol version requested by the
	 * client unless it's higher than the latest version we support. It's
	 * possible that error message fields might look different in newer
	 * protocol versions, but that's something those new clients should be
	 * able to deal with.
	 */
	FrontendProtocol = Min(proto, PG_PROTOCOL_LATEST);

	/* Check that the major protocol version is in range. */
	if (PG_PROTOCOL_MAJOR(proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
		PG_PROTOCOL_MAJOR(proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST))
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported frontend protocol %u.%u: server supports %u.0 to %u.%u",
						PG_PROTOCOL_MAJOR(proto), PG_PROTOCOL_MINOR(proto),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST),
						PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST))));

	/*
	 * Now fetch parameters out of startup packet and save them into the Port
	 * structure.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Handle protocol version 3 startup packet */
	{
		int32		offset = sizeof(ProtocolVersion);
		List	   *unrecognized_protocol_options = NIL;

		/*
		 * Scan packet body for name/option pairs.  We can assume any string
		 * beginning within the packet body is null-terminated, thanks to
		 * zeroing extra byte above.
		 */
		port->guc_options = NIL;

		while (offset < len)
		{
			char	   *nameptr = buf + offset;
			int32		valoffset;
			char	   *valptr;

			if (*nameptr == '\0')
				break;			/* found packet terminator */
			valoffset = offset + strlen(nameptr) + 1;
			if (valoffset >= len)
				break;			/* missing value, will complain below */
			valptr = buf + valoffset;

			if (strcmp(nameptr, "database") == 0)
				port->database_name = pstrdup(valptr);
			else if (strcmp(nameptr, "user") == 0)
				port->user_name = pstrdup(valptr);
			else if (strcmp(nameptr, "options") == 0)
				port->cmdline_options = pstrdup(valptr);
			else if (strcmp(nameptr, "replication") == 0)
			{
				/*
				 * Due to backward compatibility concerns the replication
				 * parameter is a hybrid beast which allows the value to be
				 * either boolean or the string 'database'. The latter
				 * connects to a specific database which is e.g. required for
				 * logical decoding while.
				 */
				if (strcmp(valptr, "database") == 0)
				{
					am_walsender = true;
					am_db_walsender = true;
				}
				else if (!parse_bool(valptr, &am_walsender))
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									"replication",
									valptr),
							 errhint("Valid values are: \"false\", 0, \"true\", 1, \"database\".")));
			}
			else if (strncmp(nameptr, "_pq_.", 5) == 0)
			{
				/*
				 * Any option beginning with _pq_. is reserved for use as a
				 * protocol-level option, but at present no such options are
				 * defined.
				 */
				unrecognized_protocol_options =
					lappend(unrecognized_protocol_options, pstrdup(nameptr));
			}
			else
			{
				/* Assume it's a generic GUC option */
				port->guc_options = lappend(port->guc_options,
											pstrdup(nameptr));
				port->guc_options = lappend(port->guc_options,
											pstrdup(valptr));

				/*
				 * Copy application_name to port if we come across it.  This
				 * is done so we can log the application_name in the
				 * connection authorization message.  Note that the GUC would
				 * be used but we haven't gone through GUC setup yet.
				 */
				if (strcmp(nameptr, "application_name") == 0)
				{
					port->application_name = pg_clean_ascii(valptr, 0);
				}
			}
			offset = valoffset + strlen(valptr) + 1;
		}

		/*
		 * If we didn't find a packet terminator exactly at the end of the
		 * given packet length, complain.
		 */
		if (offset != len - 1)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup packet layout: expected terminator as last byte")));

		/*
		 * If the client requested a newer protocol version or if the client
		 * requested any protocol options we didn't recognize, let them know
		 * the newest minor protocol version we do support and the names of
		 * any unrecognized options.
		 */
		if (PG_PROTOCOL_MINOR(proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST) ||
			unrecognized_protocol_options != NIL)
			SendNegotiateProtocolVersion(unrecognized_protocol_options);
	}

	/* Check a user name was given. */
	if (port->user_name == NULL || port->user_name[0] == '\0')
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("no PostgreSQL user name specified in startup packet")));

	/* The database defaults to the user name. */
	if (port->database_name == NULL || port->database_name[0] == '\0')
		port->database_name = pstrdup(port->user_name);

	/*
	 * Truncate given database and user names to length of a Postgres name.
	 * This avoids lookup failures when overlength names are given.
	 */
	if (strlen(port->database_name) >= NAMEDATALEN)
		port->database_name[NAMEDATALEN - 1] = '\0';
	if (strlen(port->user_name) >= NAMEDATALEN)
		port->user_name[NAMEDATALEN - 1] = '\0';

	if (am_walsender)
		MyBackendType = B_WAL_SENDER;
	else
		MyBackendType = B_BACKEND;

	/*
	 * Normal walsender backends, e.g. for streaming replication, are not
	 * connected to a particular database. But walsenders used for logical
	 * replication need to connect to a specific database. We allow streaming
	 * replication commands to be issued even if connected to a database as it
	 * can make sense to first make a basebackup and then stream changes
	 * starting from that.
	 */
	if (am_walsender && !am_db_walsender)
		port->database_name[0] = '\0';

	/*
	 * Done filling the Port structure
	 */
	MemoryContextSwitchTo(oldcontext);

	return STATUS_OK;
}

/*
 * The client has sent a cancel request packet, not a normal
 * start-a-new-connection packet.  Perform the necessary processing.  Nothing
 * is sent back to the client.
 */
static void
ProcessCancelRequestPacket(Port *port, void *pkt, int pktlen)
{
	CancelRequestPacket *canc;
	int			len;

	if (pktlen < offsetof(CancelRequestPacket, cancelAuthCode))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of cancel request packet")));
		return;
	}
	len = pktlen - offsetof(CancelRequestPacket, cancelAuthCode);
	if (len == 0 || len > 256)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of cancel key in cancel request packet")));
		return;
	}

	canc = (CancelRequestPacket *) pkt;
	SendCancelRequest(pg_ntoh32(canc->backendPID), canc->cancelAuthCode, len);
}

/*
 * Send a NegotiateProtocolVersion to the client.  This lets the client know
 * that they have either requested a newer minor protocol version than we are
 * able to speak, or at least one protocol option that we don't understand, or
 * possibly both. FrontendProtocol has already been set to the version
 * requested by the client or the highest version we know how to speak,
 * whichever is older. If the highest version that we know how to speak is too
 * old for the client, it can abandon the connection.
 *
 * We also include in the response a list of protocol options we didn't
 * understand.  This allows clients to include optional parameters that might
 * be present either in newer protocol versions or third-party protocol
 * extensions without fear of having to reconnect if those options are not
 * understood, while at the same time making certain that the client is aware
 * of which options were actually accepted.
 */
static void
SendNegotiateProtocolVersion(List *unrecognized_protocol_options)
{
	StringInfoData buf;
	ListCell   *lc;

	pq_beginmessage(&buf, PqMsg_NegotiateProtocolVersion);
	pq_sendint32(&buf, FrontendProtocol);
	pq_sendint32(&buf, list_length(unrecognized_protocol_options));
	foreach(lc, unrecognized_protocol_options)
		pq_sendstring(&buf, lfirst(lc));
	pq_endmessage(&buf);

	/* no need to flush, some other message will follow */
}


/*
 * SIGTERM while processing startup packet.
 *
 * Running proc_exit() from a signal handler would be quite unsafe.
 * However, since we have not yet touched shared memory, we can just
 * pull the plug and exit without running any atexit handlers.
 *
 * One might be tempted to try to send a message, or log one, indicating
 * why we are disconnecting.  However, that would be quite unsafe in itself.
 * Also, it seems undesirable to provide clues about the database's state
 * to a client that has not yet completed authentication, or even sent us
 * a startup packet.
 */
static void
process_startup_packet_die(SIGNAL_ARGS)
{
	_exit(1);
}

/*
 * Timeout while processing startup packet.
 * As for process_startup_packet_die(), we exit via _exit(1).
 */
static void
StartupPacketTimeoutHandler(void)
{
	_exit(1);
}

/*
 * Helper for the log_connections GUC check hook.
 *
 * `elemlist` is a listified version of the string input passed to the
 * log_connections GUC check hook, check_log_connections().
 * check_log_connections() is responsible for cleaning up `elemlist`.
 *
 * validate_log_connections_options() returns false if an error was
 * encountered and the GUC input could not be validated and true otherwise.
 *
 * `flags` returns the flags that should be stored in the log_connections GUC
 * by its assign hook.
 */
static bool
validate_log_connections_options(List *elemlist, uint32 *flags)
{
	ListCell   *l;
	char	   *item;

	/*
	 * For backwards compatibility, we accept these tokens by themselves.
	 *
	 * Prior to PostgreSQL 18, log_connections was a boolean GUC that accepted
	 * any unambiguous substring of 'true', 'false', 'yes', 'no', 'on', and
	 * 'off'. Since log_connections became a list of strings in 18, we only
	 * accept complete option strings.
	 */
	static const struct config_enum_entry compat_options[] = {
		{"off", 0},
		{"false", 0},
		{"no", 0},
		{"0", 0},
		{"on", LOG_CONNECTION_ON},
		{"true", LOG_CONNECTION_ON},
		{"yes", LOG_CONNECTION_ON},
		{"1", LOG_CONNECTION_ON},
	};

	*flags = 0;

	/* If an empty string was passed, we're done */
	if (list_length(elemlist) == 0)
		return true;

	/*
	 * Now check for the backwards compatibility options. They must always be
	 * specified on their own, so we error out if the first option is a
	 * backwards compatibility option and other options are also specified.
	 */
	item = linitial(elemlist);

	for (size_t i = 0; i < lengthof(compat_options); i++)
	{
		struct config_enum_entry option = compat_options[i];

		if (pg_strcasecmp(item, option.name) != 0)
			continue;

		if (list_length(elemlist) > 1)
		{
			GUC_check_errdetail("Cannot specify log_connections option \"%s\" in a list with other options.",
								item);
			return false;
		}

		*flags = option.val;
		return true;
	}

	/* Now check the aspect options. The empty string was already handled */
	foreach(l, elemlist)
	{
		static const struct config_enum_entry options[] = {
			{"receipt", LOG_CONNECTION_RECEIPT},
			{"authentication", LOG_CONNECTION_AUTHENTICATION},
			{"authorization", LOG_CONNECTION_AUTHORIZATION},
			{"setup_durations", LOG_CONNECTION_SETUP_DURATIONS},
			{"all", LOG_CONNECTION_ALL},
		};

		item = lfirst(l);
		for (size_t i = 0; i < lengthof(options); i++)
		{
			struct config_enum_entry option = options[i];

			if (pg_strcasecmp(item, option.name) == 0)
			{
				*flags |= option.val;
				goto next;
			}
		}

		GUC_check_errdetail("Invalid option \"%s\".", item);
		return false;

next:	;
	}

	return true;
}


/*
 * GUC check hook for log_connections
 */
bool
check_log_connections(char **newval, void **extra, GucSource source)
{
	uint32		flags;
	char	   *rawstring;
	List	   *elemlist;
	bool		success;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("Invalid list syntax in parameter \"%s\".", "log_connections");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	/* Validation logic is all in the helper */
	success = validate_log_connections_options(elemlist, &flags);

	/* Time for cleanup */
	pfree(rawstring);
	list_free(elemlist);

	if (!success)
		return false;

	/*
	 * We succeeded, so allocate `extra` and save the flags there for use by
	 * assign_log_connections().
	 */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = flags;

	return true;
}

/*
 * GUC assign hook for log_connections
 */
void
assign_log_connections(const char *newval, void *extra)
{
	log_connections = *((int *) extra);
}
