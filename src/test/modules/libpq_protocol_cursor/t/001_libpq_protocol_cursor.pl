# Copyright (c) 2024-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my ($out, $err) = run_command(['libpq_protocol_cursor', 'tests']);
die "oops: $err" unless $err eq '';
my @tests = split(/\s+/, $out);

my $log_start = -s $node->logfile;

for my $testname (@tests)
{
	# cursor_options_without_extension must run without protocol_cursor enabled.
	# The extension does not depend on a particular minor protocol version, so
	# max_protocol_version is deliberately left at its default here.
	my $connstr = $node->connstr('postgres');
	if ($testname eq 'cursor_options_without_extension')
	{
		$connstr .= " protocol_cursor=0";
	}
	else
	{
		$connstr .= " protocol_cursor=1";
	}

	$node->command_ok(
		[
			'libpq_protocol_cursor',
			$testname,
			$connstr
		],
		"libpq_protocol_cursor $testname");
}

# A crashed backend can leave a test reporting a plain command failure, which
# does not distinguish "wrong error" from "the cluster just recovered".
ok(!$node->log_contains(qr/was terminated by signal/, $log_start),
	'no backend crashed during the cursor-option tests');

$node->stop('fast');

done_testing();
