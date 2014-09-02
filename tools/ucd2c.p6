my $cur_plane = -1;
my $last_code = -1;
my $cur_idx   = 0;

my %planes;
my @pieces;

for lines("UNIDATA/UnicodeData.txt".IO) -> $entry {
    my ($code_str, $name, $gencat, $ccclass, $bidiclass, $decmptype,
        $num1, $num2, $num3, $bidimirrored, $u1name, $isocomment,
        $suc, $slc, $stc) = $entry.split(';');

    my $code  = :16($code_str);
    my $plane = $code +> 16;
    my $idx   = $code +& 0xFFFF;

    # Is this a plane transition? If so, update planes info.
    if $cur_plane != $plane {
        %planes{$plane} = [$cur_idx, 0];
        $cur_plane = $plane;
    }

    # Otherwise handle situation where there are gaps.
    else {
        if $last_code + 1 != $code {
            for $last_code ^..^ $code {
                @pieces[$cur_idx++] = "    \{ NULL, $_, $_, $_ \}";
                %planes{$cur_plane}[1]++;
            }
        }
    }

    # Emit codepoint information.
    @pieces[$cur_idx++] =
        '    { "' ~ $name ~ '", ' ~
        :16($suc || $code_str) ~ ', ' ~
        :16($slc || $code_str) ~ ', ' ~
        :16($stc || $code_str) ~ " }";
    %planes{$cur_plane}[1]++;

    $last_code = $code;
}

# Emit prelude to the data file.
my $fh = open("src/strings/unicode.c", :w);
$fh.say('/* This file is generated by ucd2c.p6 from the Unicode database.

from http://unicode.org/copyright.html#Exhibit1 on 2012-07-20:

COPYRIGHT AND PERMISSION NOTICE

Copyright � 1991-2012 Unicode, Inc. All rights reserved. Distributed
under the Terms of Use in http://www.unicode.org/copyright.html.

Permission is hereby granted, free of charge, to any person obtaining a
copy of the Unicode data files and any associated documentation (the
"Data Files") or Unicode software and any associated documentation (the
"Software") to deal in the Data Files or Software without restriction,
including without limitation the rights to use, copy, modify, merge,
publish, distribute, and/or sell copies of the Data Files or Software,
and to permit persons to whom the Data Files or Software are furnished
to do so, provided that (a) the above copyright notice(s) and this
permission notice appear with all copies of the Data Files or Software,
(b) both the above copyright notice(s) and this permission notice appear
in associated documentation, and (c) there is clear notice in each
modified Data File or in the Software as well as in the documentation
associated with the Data File(s) or Software that the data or software
has been modified.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF
ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR HOLDERS INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR
ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER
RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or
other dealings in these Data Files or Software without prior written
authorization of the copyright holder. */

#include "moar.h"
');

# Emit the planes table.
$fh.say('static MVMUnicodePlane MVM_unicode_planes[] = {');
for %planes.sort(*.key) -> $plane {
    $fh.say('    { ' ~
        $plane.value.[0] ~ ', ' ~
        $plane.value.[1] ~ ' },');
}
$fh.say("};

#define MVM_UNICODE_PLANES %planes.elems()
");

# Emit the codepoints table.
$fh.say('static MVMCodePoint MVM_unicode_codepoints[] = {');
$fh.say(@pieces.join(",\n"));
$fh.say('};

/* Looks up address of some codepoint information. */
MVMCodePoint * MVM_unicode_codepoint_info(MVMThreadContext *tc, MVMint32 codepoint) {
    MVMint32 plane = codepoint >> 16;
    MVMint32 idx   = codepoint & 0xFFFF;
    if (plane < MVM_UNICODE_PLANES)
        if (idx < MVM_unicode_planes[plane].num_codepoints)
            return &MVM_unicode_codepoints[
                MVM_unicode_planes[plane].first_codepoint + idx];
    return NULL;
}

typedef struct _MVMUnicodeNameHashEntry {
    char *name;
    MVMint32 codepoint;
    UT_hash_handle hh;
} MVMUnicodeNameHashEntry;

/* Lazily constructed hashtable of Unicode names to codepoints.
    Okay not threadsafe since its value is deterministic. */
static MVMUnicodeNameHashEntry *codepoints_by_name = NULL;

/* Looks up a codepoint by name. Lazily constructs a hash. */
MVMint32 MVM_unicode_lookup_by_name(MVMThreadContext *tc, MVMString *name) {
    MVMuint64 size;
    char *cname = MVM_string_ascii_encode(tc, name, &size);
    MVMUnicodeNameHashEntry *result;
    if (!codepoints_by_name) {
        MVMuint32 plane;
        MVMint32 idx = 0;
        for (plane = 0; plane < MVM_UNICODE_PLANES; plane++) {
            MVMuint32 codepoint = MVM_unicode_planes[plane].first_codepoint;
            for (; codepoint < MVM_unicode_planes[plane].first_codepoint + MVM_unicode_planes[plane].num_codepoints; codepoint++) {
                MVMUnicodeNameHashEntry *entry = MVM_malloc(sizeof(MVMUnicodeNameHashEntry));
                MVMCodePoint codepoint_struct = MVM_unicode_codepoints[idx++];
                char *name = codepoint_struct.name;
                if (name) {
                    entry->name = name;
                    entry->codepoint = codepoint;
                    HASH_ADD_KEYPTR(hh, codepoints_by_name, name, strlen(name), entry);
                }
            }
        }
    }
    HASH_FIND_STR(codepoints_by_name, cname, result);
    free(cname);
    return result ? result->codepoint : -1;
}

');

# And we're done.
$fh.close();
