#!/usr/bin/perl
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Lists the data members of structs and classes without a short comment
# (doc/coding-rules.md: every member has a "//" comment at the end of its
# line, or a comment block right above it). Methods are not listed (see
# undocumented.pl), nor enum values.
# Usage: perl tools/undocumented-members.pl file...   -> "file:line: member"
use strict;
use warnings;

sub code_only {
    my ($line) = @_;
    $line =~ s/"(?:[^"\\]|\\.)*"/""/g;
    $line =~ s/'(?:[^'\\]|\\.)*'/''/g;
    $line =~ s{//.*$}{};
    return $line;
}

for my $file (@ARGV) {
    open my $in, '<', $file or die "$file: $!";
    my @lines = map { s/\r?\n$//r } <$in>;
    close $in;
    my @scope;   # 'record' for a struct / class / union body, else 'other'
    for my $i (0 .. $#lines) {
        my $line = $lines[$i];
        my $code = code_only($line);
        my $in_record = @scope && $scope[-1] eq 'record';
        # a member exposed to JS: JS_EXPORT(name) before its declaration
        my $declaration = $code =~ s/^\s*JS_EXPORT\([^)]*\)\s*//r;
        # the second line of a declaration cut in two is not a member
        my $previous_code = $i > 0 ? code_only($lines[$i - 1]) : ';';
        my $continuation = $previous_code !~ /[;{}:]\s*$/ && $previous_code =~ /\S/;
        my $is_member = $in_record && !$continuation
            && $declaration =~ /;\s*$/
            && $declaration !~ /\(/
            && $declaration !~ /^\s*(using|typedef|friend|static_assert|return|public|private|protected)\b/
            && $declaration !~ /^\s*(struct|class|enum|union)\s+\w+\s*;/
            && $declaration =~ /\w/;
        if ($is_member) {
            my $commented_after = $line =~ /\/\//;
            my $commented_above = $i > 0 && $lines[$i - 1] =~ /^\s*(\/\/|\*\/|\/\*\*)/;
            print "$file:" . ($i + 1) . ": $line\n" unless $commented_after || $commented_above;
        }
        my $opens = () = $code =~ /\{/g;
        my $closes = () = $code =~ /\}/g;
        if ($opens > $closes) {
            my $kind = $code =~ /\b(struct|class|union)\b[^;()]*\{\s*$/ ? 'record' : 'other';
            push @scope, $kind for 1 .. ($opens - $closes);
        } elsif ($closes > $opens) {
            pop @scope for 1 .. ($closes - $opens);
        }
    }
}
