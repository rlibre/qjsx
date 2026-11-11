#!/usr/bin/perl
# Style pass of the espjs C++ sources (rules of CLAUDE.md, "Coding rules"):
#   1. a lone "{" after a function signature (or a constructor initializer
#      list) goes up on the line before: "void f() {";
#   2. a block of "//" comment lines right above a function (declaration or
#      definition, at namespace / class scope, never inside a function body)
#      becomes a "/** ... */" block;
#   3. in headers (.h), the names of the one-line member / method
#      declarations of a class or struct are aligned in one column per class
#      (a * or & hangs before the column; very long types do not count);
#   4. "(T)(call(args))" loses its superfluous parentheses: "(T)call(args)";
#   0. no code on one line: control statements and bodies get braces and
#      their own lines ("case X: f(); break;" too);
#   7. every case of a switch gets its block between braces;
#   8. the cases are indented one level under their switch;
#   9. section separators become "// ### title ####" lines (column 80);
#   6. C++ casts (static_cast, reinterpret_cast, const_cast) become C casts,
#      parenthesized when a member, an index or a call follows;
#   5. in function bodies, a blank line after a block closed by a lone "}"
#      and before a loop or a switch that follows statements.
# Usage: perl tools/style.pl file...   (files rewritten in place; run it on
# common/, host/, tests/native/ and the ports, never on engine/)
use strict;
use warnings;

# Removes string and character literals (their braces do not count).
sub code_only {
    my ($line) = @_;
    $line =~ s/"(?:[^"\\]|\\.)*"/""/g;
    $line =~ s/'(?:[^'\\]|\\.)*'/''/g;
    $line =~ s{//.*$}{};
    return $line;
}

# Splits "a; b;" into its statements (not inside parentheses or literals).
sub split_statements {
    my ($text) = @_;
    my @statements;
    my $current = '';
    my $depth = 0;
    my $quote = '';
    for my $char (split //, $text) {
        $current .= $char;
        if ($quote ne '') {
            $quote = '' if $char eq $quote && $current !~ /\\.$/s;
            next;
        }
        if ($char eq '"' || $char eq "'") {
            $quote = $char;
        } elsif ($char eq '(') {
            $depth++;
        } elsif ($char eq ')') {
            $depth--;
        } elsif ($char eq ';' && $depth == 0) {
            push @statements, $current =~ s/^\s+//r;
            $current = '';
        }
    }
    push @statements, $current =~ s/^\s+//r if $current =~ /\S/;
    return @statements;
}

# Step 0: no code on one line. A control statement without braces, a body
# between braces on one line, "case X: statement;": the statements go on
# their own lines, between braces. Macro lines (ending with "\") stay.
sub expand_one_liners {
    my @out;
    my $parens = qr/(\((?:[^()]++|(?-1))*\))/;
    for my $line (@_) {
        if ($line =~ /\\\s*$/) {
            push @out, $line;
            next;
        }
        # if (x) statement;   for (...) statement;   else statement;
        if ($line =~ /^(\s*)((?:\}\s*)?(?:else\s+)?(?:if|for|while)\s*$parens)\s*([^{\s].*;)\s*$/
            && $line !~ /^\s*\}\s*while\b/) {
            my ($indent, $head, $body) = ($1, $2, $4);
            push @out, "$indent$head {";
            push @out, map { "$indent    $_" } split_statements($body);
            push @out, "$indent}";
            next;
        }
        if ($line =~ /^(\s*)((?:\}\s*)?else)\s+([^{\s].*;)\s*$/ && $line !~ /\belse\s+if\b/) {
            my ($indent, $head, $body) = ($1, $2, $3);
            push @out, "$indent$head {";
            push @out, map { "$indent    $_" } split_statements($body);
            push @out, "$indent}";
            next;
        }
        # head { statements; }
        if ($line =~ /^(\s*)(.*?(?:\)|\bconst|\boverride|\bnoexcept|\belse|\bdo))\s*\{\s*([^{}]*;)\s*\}\s*(;?)\s*$/
            && $line !~ /^\s*(struct|class|union|enum|namespace)\b/
            && $line !~ /=\s*\{/) {
            my ($indent, $head, $body, $semicolon) = ($1, $2, $3, $4);
            push @out, "$indent$head {";
            push @out, map { "$indent    $_" } split_statements($body);
            push @out, "$indent}$semicolon";
            next;
        }
        # case X: statements;
        if ($line =~ /^(\s*)((?:case\s+[^:]+(?:::[^:]+)*|default)\s*:)\s+([^{\s\/].*;)\s*$/) {
            my ($indent, $label, $body) = ($1, $2, $3);
            push @out, "$indent$label";
            push @out, map { "$indent    $_" } split_statements($body);
            next;
        }
        push @out, $line;
    }
    return @out;
}

# Step 7: every case of a switch gets its block between braces:
#     case X: {
#         ...
#         break;
#     }
# A label without statements (falls through to the next one) stays as it
# is; a label that already opens a block too. Macro lines are left alone.
sub brace_cases {
    my @lines = @_;
    my @out;
    my $i = 0;
    while ($i <= $#lines) {
        my $line = $lines[$i];
        my ($indent, $head, $comment) = $line =~ /^(\s*)(case\b.*?|default\s*):\s*(\/\/.*)?$/;
        my $is_label = defined $indent && $line !~ /\{\s*(\/\/.*)?$/ && $line !~ /\\\s*$/;
        if (!$is_label) {
            push @out, $line;
            $i++;
            next;
        }

        # the statements of the case: the lines indented deeper than the label
        my $end = $i + 1;
        while ($end <= $#lines && ($lines[$end] =~ /^\s*$/ || $lines[$end] =~ /^\Q$indent\E\s+\S/)) {
            $end++;
        }

        # trailing blank lines stay outside the block
        my $last = $end - 1;
        $last-- while $last > $i && $lines[$last] =~ /^\s*$/;

        if ($last == $i) {
            push @out, $line;   # no statement: falls through
            $i++;
            next;
        }

        my $trailing = defined $comment ? "   $comment" : '';
        push @out, "$indent$head: {$trailing";
        push @out, @lines[$i + 1 .. $last];
        push @out, "$indent}";
        $i = $last + 1;
    }
    return @out;
}

# Step 8: the cases of a switch are indented one level under it:
#     switch (kind) {
#         case X: {
#             ...
#         }
#     }
# A switch whose first case is already indented is left alone (the step can
# run again without shifting twice). Nested switches are handled the same.
sub indent_cases {
    my @lines = @_;
    my @out;
    my @shifts;   # one entry per switch being shifted: its brace depth at opening
    my $depth = 0;
    for my $i (0 .. $#lines) {
        my $line = $lines[$i];
        my $code = code_only($line);
        my $opens = () = $code =~ /\{/g;
        my $closes = () = $code =~ /\}/g;

        # the closing brace of a shifted switch ends its shift (before printing it)
        my $depth_after = $depth + $opens - $closes;
        while (@shifts && $depth_after <= $shifts[-1] && $closes > 0) {
            pop @shifts;
        }

        my $extra = '    ' x scalar(@shifts);
        push @out, $line =~ /^\s*$/ ? $line : $extra . $line;

        # a switch opening here: shift its body if its first label is at its level
        if ($line =~ /^(\s*)switch\s*\(.*\)\s*\{\s*$/) {
            my $indent = $1;
            my $next = $i + 1;
            $next++ while $next <= $#lines && $lines[$next] =~ /^\s*$/;
            my $label_at_switch_level = $next <= $#lines && $lines[$next] =~ /^\Q$indent\E(case\b|default\b)/;
            push @shifts, $depth if $label_at_switch_level;
        }

        $depth = $depth_after;
    }
    return @out;
}

# Step 9: section separators are lines of '#' up to column 80:
#     // ### Sending ###########...      (titled)
#     // ##########################      (bare)
# Old forms are converted: "// --- title ---", "// -------" and a dash line
# followed by a one-line title ("// Sending").
my $SEPARATOR_WIDTH = 80;

sub separator_line {
    my ($indent, $title) = @_;
    my $start = defined $title ? "$indent// ### $title " : "$indent// ";
    my $fill = $SEPARATOR_WIDTH - length($start);
    $fill = 3 if $fill < 3;
    return $start . ('#' x $fill);
}

sub separators {
    my @lines = @_;
    my @out;
    my $i = 0;
    while ($i <= $#lines) {
        my $line = $lines[$i];

        # bare dash or hash line, maybe followed by a one-line title
        if ($line =~ /^(\s*)\/\/\s*([-#])\2{2,}\s*$/) {
            my $indent = $1;
            my $next = $i < $#lines ? $lines[$i + 1] : '';
            my $after = $i + 1 < $#lines ? $lines[$i + 2] : '';
            my ($title) = $next =~ /^\Q$indent\E\/\/\s+(\S.{0,60}?)\s*$/;
            my $single_title = defined $title && $title !~ /^[-#]/ && $after !~ /^\s*\/\//;
            if ($single_title) {
                push @out, separator_line($indent, $title);
                $i += 2;
                next;
            }

            push @out, separator_line($indent, undef);
            $i++;
            next;
        }

        # titled: "// --- title ---" or "// ### title ###"
        if ($line =~ /^(\s*)\/\/\s*(?:-{3,}|#{3,})\s+(.+?)\s*(?:-+|#+)?\s*$/) {
            push @out, separator_line($1, $2);
            $i++;
            next;
        }

        push @out, $line;
        $i++;
    }
    return @out;
}

# Step 1: joins "{" alone on a line to the signature above it.
sub join_braces {
    my @lines = @_;
    my @out;
    for my $line (@lines) {
        my $previous = @out ? $out[-1] : undef;
        my $lone_brace = $line =~ /^\s*\{\s*$/;
        my $after_signature = defined $previous
            && code_only($previous) =~ /(\)|\bconst|\boverride|\bnoexcept|\bfinal)\s*$/
            && $previous !~ /^\s*(if|for|while|switch|else|do)\b/;
        my $after_initializer = defined $previous && $previous =~ /^\s*:\s*\w/ && code_only($previous) =~ /\)\s*$/;
        if ($lone_brace && ($after_signature || $after_initializer)) {
            $out[-1] =~ s/\s*$/ {/;
            next;
        }
        push @out, $line;
    }
    return @out;
}

# Kind of scope opened by a line ending with "{": 'type' for namespaces,
# classes, structs, unions, enums and extern "C"; 'code' otherwise.
sub scope_kind {
    my ($code) = @_;
    return 'type' if $code =~ /\b(namespace|class|struct|union|enum)\b[^;()]*\{\s*$/;
    return 'type' if $code =~ /^\s*extern\s*""\s*\{\s*$/;
    return 'code';
}

# Does this line start a function declaration or definition?
sub is_function_line {
    my ($line) = @_;
    return 0 if $line =~ /^\s*:/;                    # constructor initializer list
    return 0 if $line =~ /^\s*(return|else|delete|new|throw|case|goto|if|for|while|switch|do|using|typedef|friend)\b/;
    # an "=" before the parameters is an assignment, not a declaration
    my ($before_parameters) = $line =~ /^([^(]*)/;
    return 0 if $before_parameters =~ /=/ && $line !~ /operator\s*=/;
    return 1 if $line =~ /^\s*template\s*</;
    # [qualifiers / type...] name (
    return 1 if $line =~ /^\s*(?:(?:static|inline|virtual|explicit|constexpr|extern)\s+)*(?:[\w:<>,]+[\s*&]+)+~?[\w:]+\s*\(/;
    # constructors and destructors: Name::Name( / Name( / ~Name(
    return 1 if $line =~ /^\s*(?:explicit\s+)?~?[A-Z]\w*(?:::~?[A-Z]\w*)?\s*\(/;
    return 0;
}

# Steps 2 and 3, with the scope of every line known.
sub comments_and_alignment {
    my ($is_header, @lines) = @_;
    my @scope;          # stack of 'type' / 'code'
    my @in_code;        # per line: inside a function body?
    for my $i (0 .. $#lines) {
        $in_code[$i] = grep({ $_ eq 'code' } @scope) ? 1 : 0;
        my $code = code_only($lines[$i]);
        my $opens = () = $code =~ /\{/g;
        my $closes = () = $code =~ /\}/g;
        if ($opens > $closes) {
            push @scope, scope_kind($code) for 1 .. ($opens - $closes);
        } elsif ($closes > $opens) {
            pop @scope for 1 .. ($closes - $opens);
        }
    }

    # step 2: "//" blocks above functions
    my @out;
    my $i = 0;
    while ($i <= $#lines) {
        if (!$in_code[$i] && $lines[$i] =~ /^(\s*)\/\/ ?(.*)$/ && $lines[$i] !~ /^\s*\/\/\s*(-{3}|#{3})/) {
            my $indent = $1;
            my $start = $i;
            my @text;
            while ($i <= $#lines && $lines[$i] =~ /^\Q$indent\E\/\/ ?(.*)$/) {
                push @text, $1;
                $i++;
            }
            my $next = $i <= $#lines ? $lines[$i] : '';
            my $same_indent = $next =~ /^\Q$indent\E\S/;
            if ($same_indent && !$in_code[$i] && is_function_line($next)) {
                push @out, "$indent/**";
                push @out, map { $_ eq '' ? "$indent *" : "$indent * $_" } @text;
                push @out, "$indent */";
            } else {
                push @out, @lines[$start .. $i - 1];
            }
            next;
        }
        push @out, $lines[$i];
        $i++;
    }
    @out = separate_blocks(@out);
    return @out unless $is_header;

    # step 3: alignment of declarations inside classes / structs (headers)
    my @scope2;
    my @class_ids;   # parallel to @scope2: number of each class body
    my $next_class = 0;
    my @kinds;       # per line: 'member' if a one-line declaration in a class
    my @parts;
    my @class_of;    # per member line: number of its class body
    for my $j (0 .. $#out) {
        my $line = $out[$j];
        my $in_class = @scope2 && $scope2[-1] eq 'class';
        $kinds[$j] = '';
        $class_of[$j] = $in_class ? $class_ids[-1] : -1;
        if ($in_class) {
            my $code = code_only($line);
            my $ok = $line !~ /^\s*(public|private|protected)\s*:/
                && $line !~ /^\s*(template|friend|using|typedef|return|static_assert|explicit|struct|class|enum|union)\b/
                && $line !~ /operator/
                && ($code =~ /;\s*$/ || $code =~ /\}\s*$/);
            # indent, type, name (with its * or &), rest
            if ($ok && $line =~ /^(\s+)((?:(?:static|inline|virtual|explicit|constexpr|const|mutable|unsigned|signed)\s+)*[\w:]+(?:<[^;(){}]*>)?(?:\s+const)?(?:\s*[*&]+)?)\s+([*&]*~?\w+)(\s*(?:\(|;|=|\[|\{|:).*)$/) {
                my ($indent, $type, $name, $rest) = ($1, $2, $3, $4);
                if ($type =~ s/\s*([*&]+)$//) {
                    $name = $1 . $name;   # the * or & stays with the name
                }
                $kinds[$j] = 'member';
                $parts[$j] = [$indent, $type, $name, $rest];
            }
        }
        my $code = code_only($line);
        my $opens = () = $code =~ /\{/g;
        my $closes = () = $code =~ /\}/g;
        if ($opens > $closes) {
            my $kind = $code =~ /\b(class|struct|union)\b[^;()]*\{\s*$/ ? 'class' : scope_kind($code);
            for (1 .. ($opens - $closes)) {
                push @scope2, $kind;
                push @class_ids, $next_class++;
            }
        } elsif ($closes > $opens) {
            for (1 .. ($closes - $opens)) {
                pop @scope2;
                pop @class_ids;
            }
        }
    }
    # one column per class body: the members of a class are the 'member'
    # lines with the same class number; types longer than MAX_TYPE do not
    # push the column (they keep one space)
    # the * or & of a name hangs before the column: "Message  *take"
    my $MAX_TYPE = 24;
    my %width;
    for my $k (0 .. $#out) {
        next unless $kinds[$k] eq 'member';
        my ($stars) = $parts[$k][2] =~ /^([*&]*)/;
        my $length = length($parts[$k][1]) + length($stars);
        next if length($parts[$k][1]) > $MAX_TYPE;
        my $key = $class_of[$k];
        $width{$key} = $length if !defined $width{$key} || $length > $width{$key};
    }
    for my $k (0 .. $#out) {
        next unless $kinds[$k] eq 'member';
        my ($ind, $type, $name, $rest) = @{$parts[$k]};
        my ($stars) = $name =~ /^([*&]*)/;
        my $width = $width{$class_of[$k]} // 0;
        my $spaces = $width - length($type) - length($stars) + 1;
        $spaces = 1 if $spaces < 1;
        $out[$k] = $ind . $type . (' ' x $spaces) . $name . $rest;
    }
    return @out;
}

# Step 5: blank lines between the parts of a function body: after a block
# closed by a lone "}" (unless "}", else, catch or the while of a do
# follows), and before a loop or a switch that follows statements.
sub separate_blocks {
    my @lines = @_;
    my @scope;
    my @in_code;
    for my $i (0 .. $#lines) {
        $in_code[$i] = grep({ $_ eq 'code' } @scope) ? 1 : 0;
        my $code = code_only($lines[$i]);
        my $opens = () = $code =~ /\{/g;
        my $closes = () = $code =~ /\}/g;
        if ($opens > $closes) {
            push @scope, scope_kind($code) for 1 .. ($opens - $closes);
        } elsif ($closes > $opens) {
            pop @scope for 1 .. ($closes - $opens);
        }
    }
    my @out;
    for my $i (0 .. $#lines) {
        my $line = $lines[$i];
        my $previous = $i > 0 ? $lines[$i - 1] : '';
        my $previous_blank = $previous =~ /^\s*$/;
        # between the methods of a class: a blank line after a body
        my $after_method = $i > 0 && !$in_code[$i] && !$previous_blank && $previous =~ /^\s*\}\s*$/
            && $line !~ /^\s*(\}|$)/ && $line !~ /^\s*(public|private|protected)\s*:/;
        push @out, '' if $after_method;
        if ($i > 0 && $in_code[$i] && $in_code[$i - 1] && !$previous_blank && $line !~ /^\s*$/) {
            my $closes_block = $previous =~ /^\s*\}\s*$/ || $previous =~ /^\s*\}\s*while\b.*;\s*$/;
            my $after_block = $closes_block
                && $line !~ /^\s*(\}|else\b|catch\b|case\b|default\b)/
                && $line !~ /^\s*while\b.*;\s*$/;
            my $loop = $line =~ /^\s*(while|for|do|switch)\b/
                && $previous !~ /[{:]\s*$/
                && $previous !~ /^\s*(\/\/|\/\*|\*)/;
            push @out, '' if $after_block || $loop;
        }
        push @out, $line;
    }
    return @out;
}

# Step 6: C++ casts (static_cast, reinterpret_cast, const_cast) become C
# casts. "(T)value" when the value is a plain name or member chain, else
# "(T)(value)"; the whole cast is parenthesized when a member access, an
# index or a call follows ("((Runner *)opaque)->run()").
my $cast_type = qr/[^<>()]*(?:<[^<>]*(?:<[^<>]*>[^<>]*)*>[^<>()]*)*/;
my $cast_parens;
$cast_parens = qr/\((?:[^()]++|(??{$cast_parens}))*\)/;

sub c_casts {
    my ($line) = @_;
    1 while $line =~ s{(?:static|reinterpret|const)_cast<($cast_type)>($cast_parens)(?=(->|\.|\[|\()?)}{
        my ($type, $operand, $follower) = ($1, $2, $3);
        my $inner = substr($operand, 1, -1);
        my $simple = $inner =~ /^\s*[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*\s*$/;
        my $cast = $simple ? "($type)$inner" : "($type)$operand";
        defined $follower ? "($cast)" : $cast;
    }e;
    return $line;
}

for my $file (@ARGV) {
    open my $in, '<', $file or die "$file: $!";
    my @lines = map { s/\r?\n$//r } <$in>;
    close $in;
    @lines = separators(@lines);
    @lines = expand_one_liners(@lines);
    # nested switches: again until nothing changes
    my $before = '';
    while (join("
", @lines) ne $before) {
        $before = join("
", @lines);
        @lines = brace_cases(@lines);
    }
    @lines = indent_cases(@lines);
    @lines = join_braces(@lines);
    @lines = comments_and_alignment($file =~ /\.h$/ ? 1 : 0, @lines);
    for (@lines) {
        $_ = c_casts($_);
        # step 4
        s/\(((?:const\s+)?[A-Za-z_][\w:<>]*(?:\s*\*+)?)\)\((\w+(?:::\w+)*\([^()]*\))\)/($1)$2/g;
    }
    open my $outfh, '>', $file or die "$file: $!";
    print $outfh "$_\n" for @lines;
    close $outfh;
}
