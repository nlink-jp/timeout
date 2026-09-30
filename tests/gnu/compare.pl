#!/usr/bin/perl
# compare.pl EXPECTED OURS GNU — fail on any difference between the two
# runner.pl outputs that is not listed in EXPECTED, and on any listed
# difference that is not observed exactly as listed. The listed differences
# also serve as the positive control: a harness that compared nothing (or
# compared a file with itself) could not reproduce them.
use strict;
use warnings;

my ($expected_file, $ours_file, $gnu_file) = @ARGV;
die "usage: compare.pl EXPECTED OURS GNU\n" unless defined $gnu_file;

sub read_outcomes {
	my ($file) = @_;
	open my $fh, '<', $file or die "$file: $!\n";
	my (%meta, @ids, %outcome);
	while (<$fh>) {
		chomp;
		my ($k, $v) = split /\t/, $_, 2;
		die "$file: malformed line: $_\n" unless defined $v;
		if ($k =~ s/^# //) {
			$meta{$k} = $v;
			next;
		}
		die "$file: duplicate case $k\n" if exists $outcome{$k};
		push @ids, $k;
		$outcome{$k} = $v;
	}
	return (\%meta, \@ids, \%outcome);
}

my ($om, $oids, $ours) = read_outcomes($ours_file);
my ($gm, $gids, $gnu) = read_outcomes($gnu_file);
my @fail;

# The harness checks itself first.
push @fail, "ours is not this project's timeout: '$om->{version}'"
	unless ($om->{version} // '') =~ /^timeout version /;
push @fail, "the oracle is not GNU coreutils timeout: '$gm->{version}'"
	unless ($gm->{version} // '') =~ /\(GNU coreutils\) /;
push @fail, "case counts differ or are empty: ours $om->{cases}, GNU $gm->{cases}"
	unless ($om->{cases} // 0) > 0 && $om->{cases} == ($gm->{cases} // -1)
	&& $om->{cases} == @$oids && $gm->{cases} == @$gids;
push @fail, "case lists differ" unless join("\n", @$oids) eq join("\n", @$gids);

my %expected;
open my $ef, '<', $expected_file or die "$expected_file: $!\n";
while (<$ef>) {
	chomp;
	next if /^\s*(#|$)/;
	my ($id, $o, $g, $why) = split /\t/;
	die "$expected_file: malformed line: $_\n" unless defined $why;
	$expected{$id} = [ $o, $g, $why ];
}

my ($same, $known) = (0, 0);
for my $id (@$oids) {
	my ($o, $g) = ($ours->{$id}, $gnu->{$id} // '(missing)');
	if (my $e = $expected{$id}) {
		if ($o eq $e->[0] && $g eq $e->[1]) {
			$known++;
		} else {
			push @fail, "$id: expected ours=$e->[0] GNU=$e->[1] ($e->[2]); got ours=$o GNU=$g";
		}
	} elsif ($o eq $g) {
		$same++;
	} else {
		push @fail, "$id: ours=$o GNU=$g — an unlisted difference";
	}
}
for my $id (sort keys %expected) {
	push @fail, "$id: listed in $expected_file but not a case" unless exists $ours->{$id};
}
push @fail, "no listed difference was observed: the comparison proves nothing" unless $known > 0;

print "test-gnu: $same cases identical, $known known differences (ours: $om->{version}; oracle: $gm->{version})\n";
if (@fail) {
	print "test-gnu: FAIL\n";
	print "  $_\n" for @fail;
	exit 1;
}
print "test-gnu: OK\n";
