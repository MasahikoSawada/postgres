
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_controldata');
program_version_ok('pg_controldata');
program_options_handling_ok('pg_controldata');
command_fails(['pg_controldata'], 'pg_controldata without arguments fails');
command_fails([ 'pg_controldata', 'nonexistent' ],
	'pg_controldata with nonexistent directory fails');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

command_like([ 'pg_controldata', $node->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');


# check with a corrupted pg_control

my $pg_control = $node->data_dir . '/global/pg_control';
my $size = -s $pg_control;

open my $fh, '>', $pg_control or BAIL_OUT($!);
binmode $fh;

# fill file with zeros
print $fh pack("x[$size]");
close $fh;

command_checks_all(
	[ 'pg_controldata', $node->data_dir ],
	0,
	[qr/./],
	[
		qr/warning: calculated CRC checksum does not match value stored in control file/,
		qr/warning: invalid WAL segment size/
	],
	'pg_controldata with corrupted pg_control');

done_testing();
