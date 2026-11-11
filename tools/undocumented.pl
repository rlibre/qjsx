#!/usr/bin/perl
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Lists the functions without a "/** */" comment (CLAUDE.md, "Coding rules":
# every function and method has one). A function is a declaration or a
# definition at namespace or class scope (never a statement of a body); its
# comment is the block that ends right above it (a template<> line may sit
# between them).
# Usage: perl tools/undocumented.pl file...   -> "file:line: signature"
use strict;
use warnings;

sub code_only {
    my ($line) = @_;
    $line =~ s/"(?:[^"\\]|\\.)*"/""/g;
    $line =~ s/'(?:[^'\\]|\\.)*'/''/g;
    $line =~ s{//.*$}{};
    return $line;
}

sub scope_kind {
    my ($code) = @_;
    return 'type' if $code =~ /\b(namespace|class|struct|union|enum)\b[^;()]*\{\s*$/;
    return 'type' if $code =~ /^\s*extern\s*""\s*\{\s*$/;
    return 'code';
}

sub is_function_line {
    my ($line) = @_;
    return 0 if $line =~ /^\s*:/;                    # constructor initializer list
    return 0 if $line =~ /^\s*(return|else|delete|new|throw|case|goto|if|for|while|switch|do|using|typedef|friend|template)\b/;
    return 0 if $line =~ /=\s*delete\s*;|=\s*default\s*;/;
    return 0 if $line =~ /\\\s*$/;                       # macro body
    return 0 if $line =~ /^\s*[A-Z_][A-Z0-9_]*\s*\(/;   # macro use (JS_MODULE, JS_EXPORT...)
    my ($before_parameters) = $line =~ /^([^(]*)/;
    return 0 if $before_parameters =~ /=/;
    return 1 if $line =~ /^\s*(?:(?:static|inline|virtual|explicit|constexpr|extern)\s+)*(?:[\w:<>,]+[\s*&]+)+~?[\w:]+\s*\(/;
    return 1 if $line =~ /^\s*(?:explicit\s+)?~?[A-Z]\w*(?:::~?[A-Z]\w*)?\s*\(/;
    return 0;
}

for my $file (@ARGV) {
    open my $in, '<', $file or die "$file: $!";
    my @lines = map { s/\r?\n$//r } <$in>;
    close $in;
    my @scope;
    for my $i (0 .. $#lines) {
        my $in_code = grep({ $_ eq 'code' } @scope) ? 1 : 0;
        my $line = $lines[$i];
        if (!$in_code && is_function_line($line)) {
            my $j = $i - 1;
            $j-- while $j >= 0 && $lines[$j] =~ /^\s*template\s*</;
            my $documented = $j >= 0 && $lines[$j] =~ /\*\/\s*$/;
            print "$file:" . ($i + 1) . ": $line\n" unless $documented;
        }
        my $code = code_only($line);
        my $opens = () = $code =~ /\{/g;
        my $closes = () = $code =~ /\}/g;
        if ($opens > $closes) {
            push @scope, scope_kind($code) for 1 .. ($opens - $closes);
        } elsif ($closes > $opens) {
            pop @scope for 1 .. ($closes - $opens);
        }
    }
}
