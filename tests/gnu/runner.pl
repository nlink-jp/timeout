#!/usr/bin/perl
# runner.pl — run every case against the timeout named by $TIMEOUT and print
# one line per case: "<id>\t<outcome>", where outcome is "exit:N" or
# "sig:NAME". The same file runs on macOS (this project's timeout) and in a
# Linux container (GNU coreutils timeout); compare.pl diffs the two.
#
# Only outcomes that do not depend on the platform are compared: signal
# numbers differ between Linux and macOS, so -s uses numbers only where they
# agree (1 2 9 15), and signals are reported by name.
use strict;
use warnings;
use Config;
use File::Temp qw(tempdir);

my $timeout = $ENV{TIMEOUT} or die "runner.pl: set TIMEOUT to the binary under test\n";
my @signame = split ' ', $Config{sig_name};

my $dir = tempdir(CLEANUP => 1);
sub fixture {
	my ($name, $text, $mode) = @_;
	open my $fh, '>', "$dir/$name" or die "$dir/$name: $!\n";
	print $fh $text;
	close $fh;
	chmod $mode, "$dir/$name";
}
fixture('noshebang', "exit 3\n", 0755);
fixture('notexec', "exit 0\n", 0644);

my $ignore_term = q{trap "" TERM; exec sleep 1};
my $catch_term = q{trap "exit 5" TERM; sleep 5 & wait};

my @cases = (
	# exit statuses
	[ 'exit-0', '5', 'true' ],
	[ 'exit-7', '5', 'sh', '-c', 'exit 7' ],
	[ 'not-found', '5', 'timeout-gnu-no-such-command' ],
	[ 'not-executable', '5', "$dir/notexec" ],
	[ 'no-shebang', '5', "$dir/noshebang" ],
	[ 'timed-out', '0.2', 'sleep', '5' ],
	[ 'child-signal', '5', 'sh', '-c', 'kill -USR1 $$' ],
	[ 'options-stop', '5', 'ls', '-l', '/' ],
	[ 'missing-command', '5' ],
	[ 'missing-operands' ],
	[ 'help', '--help' ],
	[ 'version', '--version' ],

	# signals and options
	[ 's-TERM', '-s', 'TERM', '0.2', 'sleep', '5' ],
	[ 's-term', '-s', 'term', '0.2', 'sleep', '5' ],
	[ 's-SIGTERM', '-s', 'SIGTERM', '0.2', 'sleep', '5' ],
	[ 's-sigterm', '-s', 'sigterm', '0.2', 'sleep', '5' ],
	[ 's-INT', '-s', 'INT', '0.2', 'sleep', '5' ],
	[ 's-HUP', '-s', 'HUP', '0.2', 'sleep', '5' ],
	[ 's-USR1', '-s', 'USR1', '0.2', 'sleep', '5' ],
	[ 's-ALRM', '-s', 'ALRM', '0.2', 'sleep', '5' ],
	[ 's-CONT', '-s', 'CONT', '0.2', 'sleep', '1' ],
	[ 's-1', '-s', '1', '0.2', 'sleep', '5' ],
	[ 's-2', '-s', '2', '0.2', 'sleep', '5' ],
	[ 's-15', '-s', '15', '0.2', 'sleep', '5' ],
	[ 's-01', '-s', '01', '0.2', 'sleep', '5' ],
	[ 's-0', '-s', '0', '0.2', 'sleep', '1' ],
	[ 's-64', '-s', '64', '0.2', 'sleep', '1' ],
	[ 's-minus9', '-s', '-9', '0.2', 'sleep', '1' ],
	[ 's-9x', '-s', '9x', '0.2', 'sleep', '1' ],
	[ 's-empty', '-s', '', '0.2', 'sleep', '1' ],
	[ 's-SIG', '-s', 'SIG', '0.2', 'sleep', '1' ],
	[ 's-SIGSIGTERM', '-s', 'SIGSIGTERM', '0.2', 'sleep', '1' ],
	[ 's-NOPE', '-s', 'NOPE', '0.2', 'sleep', '1' ],
	[ 's-attached', '-sINT', '0.2', 'sleep', '5' ],
	[ 'signal-eq', '--signal=INT', '0.2', 'sleep', '5' ],
	[ 'signal-sep', '--signal', 'INT', '0.2', 'sleep', '5' ],
	[ 's-KILL', '-s', 'KILL', '0.2', 'sleep', '5' ],
	[ 's-9', '-s', '9', '0.2', 'sleep', '5' ],
	[ 'f-s-KILL', '-f', '-s', 'KILL', '0.2', 'sleep', '5' ],
	[ 'k', '-k', '0.2', '0.2', 'sh', '-c', $ignore_term ],
	[ 'k-attached', '-k0.2', '0.2', 'sh', '-c', $ignore_term ],
	[ 'k-eq', '--kill-after=0.2', '0.2', 'sh', '-c', $ignore_term ],
	[ 'k-sep', '--kill-after', '0.2', '0.2', 'sh', '-c', $ignore_term ],
	[ 'k-zero', '-k', '0', '0.2', 'sh', '-c', $ignore_term ],
	[ 'k-bad', '-k', 'x', '0.2', 'true' ],
	[ 'f-k', '-f', '-k', '0.2', '0.2', 'sh', '-c', $ignore_term ],
	[ 'f', '-f', '0.2', 'sleep', '5' ],
	[ 'foreground', '--foreground', '0.2', 'sleep', '5' ],
	[ 'p-signal', '-p', '0.2', 'sleep', '5' ],
	[ 'p-exit', '-p', '0.2', 'sh', '-c', $catch_term ],
	[ 'no-p-exit', '0.2', 'sh', '-c', $catch_term ],
	[ 'preserve-status', '--preserve-status', '0.2', 'sleep', '5' ],
	[ 'abbrev-pres', '--pres', '5', 'true' ],
	[ 'abbrev-fore', '--fore', '5', 'true' ],
	[ 'abbrev-verb', '--verb', '5', 'true' ],
	[ 'abbrev-ambiguous', '--ver', '5', 'true' ],
	[ 'verbose', '-v', '0.2', 'sleep', '5' ],
	[ 'unknown-long', '--nope', '5', 'true' ],
	[ 'unknown-short', '-x', '5', 'true' ],
	[ 'double-dash', '--', '5', 'true' ],
	[ 'minus0-as-option', '-0', 'true' ],

	# signals sent to timeout while it runs: { send => SIGNAL, after => seconds }
	[ 'fwd-TERM', { send => 'TERM', after => 0.3 }, '5', 'sleep', '5' ],
	[ 'fwd-USR1-no-limit', { send => 'USR1', after => 0.3 }, '0', 'sleep', '5' ],
	[ 'fwd-k-no-limit', { send => 'TERM', after => 0.3 }, '-k', '0.3', '0', 'sh', '-c', q{trap "" TERM; exec sleep 2} ],
	[ 's-0-verbose-name', '-s', 'EXIT', '0.2', 'sleep', '1' ],

	# durations with a limit that trips at once
	[ 'tiny', '--', '1e-9', 'sleep', '5' ],
	[ 'underflow', '--', '1e-400', 'sleep', '5' ],
	[ 'hex-fraction', '0x1p-3', 'sleep', '5' ],
);

# Durations accepted or rejected (with a command that exits at once).
for my $d ('0', '-0', '0s', '1', '1s', '1m', '1h', '1d', '1.5', '.5', '1.', '+2', '1e3',
	' 3', '0x10', '0x1d', '0x1p0', '0x1p0d', '0X1P-1', '1e-10', '1e400', 'inf',
	'INFINITY', '2147483647', '3e9', '24856d', 'nan', 'NaN', '', '-1', '-1e-9', '1x',
	'1ss', '1 ', '1 s', 's', 'abc', '0x', '1_000', '1,5', '--1', '1S', '1e', '0x1p') {
	(my $id = "d[$d]") =~ s/ /_/g;
	push @cases, [ $id, '--', $d, 'true' ];
}

my $version = `"$timeout" --version 2>&1`;
$version =~ s/\n.*//s;
print "# version\t$version\n";
print "# cases\t", scalar(@cases), "\n";

for my $c (@cases) {
	my ($id, @args) = @$c;
	my $send = ref $args[0] eq 'HASH' ? shift @args : undef;
	my $pid = fork;
	die "fork: $!\n" unless defined $pid;
	if ($pid == 0) {
		open STDIN, '<', '/dev/null';
		open STDOUT, '>', '/dev/null';
		open STDERR, '>', '/dev/null';
		exec { $timeout } $timeout, @args;
		exit 99;
	}
	if ($send) {
		select(undef, undef, undef, $send->{after});
		kill $send->{send}, $pid;
	}
	waitpid $pid, 0;
	my $st = $?;
	my $outcome = ($st & 127) ? 'sig:' . $signame[$st & 127] : 'exit:' . ($st >> 8);
	print "$id\t$outcome\n";
}
