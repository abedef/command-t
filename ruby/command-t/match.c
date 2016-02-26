// Copyright 2010-present Greg Hurrell. All rights reserved.
// Licensed under the terms of the BSD 2-clause license.

#include <float.h> /* for DBL_MAX */
#include "match.h"
#include "ext.h"
#include "ruby_compat.h"

#define NON_MATCH -1e9
#define UNSET DBL_MAX

// Use a struct to make passing params during recursion easier.
typedef struct {
    char    *haystack_p;            // Pointer to the path string to be searched.
    long    haystack_len;           // Length of same.
    char    *needle_p;              // Pointer to search string (needle).
    long    needle_len;             // Length of same.
    long    *rightmost_match_p;     // Rightmost match for each char in needle.
    double  max_score_per_char;
    int     always_show_dot_files;  // Boolean.
    int     never_show_dot_files;   // Boolean.
    int     case_sensitive;         // Boolean.
    int     compute_all_scorings;   // Boolean.
    double  *memo;                  // Memoization.
} matchinfo_t;

double recursive_match(
    matchinfo_t *m,    // Sharable meta-data.
    long haystack_idx, // Where in the path string to start.
    long needle_idx    // Where in the needle string to start.
) {
    double score_for_char;
    long i, distance;
    double score = NON_MATCH;

    if (needle_idx == m->needle_len) {
        // Matched whole needle in previous frame; this is the base case.
        return 0.0;
    } else if (
        needle_idx > haystack_idx ||
        haystack_idx + (m->needle_len - needle_idx) > m->rightmost_match_p[m->needle_len - 1] + 1
    ) {
        // Impossible to match here; return NON_MATCH.
        return score;
    }

    // Do we have a memoized result we can return?
    double *memoized = &m->memo[needle_idx * m->needle_len + haystack_idx];
    if (*memoized != UNSET) {
        return *memoized;
    } else if (needle_idx == m->needle_len) {
        return *memoized = 0.0;
    }

    char c = m->needle_p[needle_idx];

    for (i = haystack_idx; i <= m->rightmost_match_p[needle_idx] ; i++) {
        char d = m->haystack_p[i];
        if (d == '.') {
            if (i == 0 || m->haystack_p[i - 1] == '/') { // This is a dot-file.
                int dot_search = c == '.'; // Searching for a dot.
                if (
                    m->never_show_dot_files ||
                    (!dot_search && !m->always_show_dot_files)
                ) {
                    return *memoized = NON_MATCH;
                }
            }
        } else if (d >= 'A' && d <= 'Z' && !m->case_sensitive) {
            d += 'a' - 'A'; // Add 32 to downcase.
        }

        if (c == d) {
            // Calculate score.
            score_for_char = m->max_score_per_char;
            distance = i - haystack_idx;

            if (distance > 1) {
                double factor = 1.0;
                char last = m->haystack_p[i - 1];
                char curr = m->haystack_p[i]; // Case matters, so get again.
                if (last == '/') {
                    factor = 0.9;
                } else if (
                    last == '-' ||
                    last == '_' ||
                    last == ' ' ||
                    (last >= '0' && last <= '9')
                ) {
                    factor = 0.8;
                } else if (
                    last >= 'a' && last <= 'z' &&
                    curr >= 'A' && curr <= 'Z'
                ) {
                    factor = 0.8;
                } else if (last == '.') {
                    factor = 0.7;
                } else {
                    // If no "special" chars behind char, factor diminishes
                    // as distance from last matched char increases.
                    factor = (1.0 / distance) * 0.75;
                }
                score_for_char *= factor;
            }

            double new_score =
                score_for_char +
                recursive_match(m, i + 1, needle_idx + 1);
            if (new_score > score) {
                score = new_score;
                if (!m->compute_all_scorings) {
                    break;
                }
            }
        }
    }
    return *memoized = score;
}

double calculate_match(
    VALUE haystack,
    VALUE needle,
    VALUE case_sensitive,
    VALUE always_show_dot_files,
    VALUE never_show_dot_files,
    VALUE compute_all_scorings,
    long needle_bitmask,
    long *haystack_bitmask
) {
    matchinfo_t m;
    long i;
    double score            = 1.0;
    int compute_bitmasks    = *haystack_bitmask == 0;
    m.haystack_p            = RSTRING_PTR(haystack);
    m.haystack_len          = RSTRING_LEN(haystack);
    m.needle_p              = RSTRING_PTR(needle);
    m.needle_len            = RSTRING_LEN(needle);
    m.rightmost_match_p     = NULL;
    m.max_score_per_char    = (1.0 / m.haystack_len + 1.0 / m.needle_len) / 2;
    m.always_show_dot_files = always_show_dot_files == Qtrue;
    m.never_show_dot_files  = never_show_dot_files == Qtrue;
    m.case_sensitive        = (int)case_sensitive;
    m.compute_all_scorings  = compute_all_scorings == Qtrue;

    // Special case for zero-length search string.
    if (m.needle_len == 0) {
        // Filter out dot files.
        if (!m.always_show_dot_files) {
            for (i = 0; i < m.haystack_len; i++) {
                char c = m.haystack_p[i];
                if (c == '.' && (i == 0 || m.haystack_p[i - 1] == '/')) {
                    return 0.0;
                }
            }
        }
    } else if (m.haystack_len > 0) { // Normal case.
        if (*haystack_bitmask) {
            if ((needle_bitmask & *haystack_bitmask) != needle_bitmask) {
                return 0.0;
            }
        }

        // Pre-scan string to see if it matches at all (short-circuits).
        // Record rightmost math match for each character (used to prune search space).
        // Record bitmask for haystack to speed up future searches.
        long rightmost_match_p[m.needle_len];
        m.rightmost_match_p = rightmost_match_p;
        long needle_idx = m.needle_len - 1;
        long mask = 0;
        for (i = m.haystack_len - 1; i >= 0; i--) {
            char c = m.haystack_p[i];
            char lower = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
            if (!m.case_sensitive) {
                c = lower;
            }
            if (compute_bitmasks) {
                mask |= (1 << (lower - 'a'));
            }

            if (needle_idx >= 0) {
                char d = m.needle_p[needle_idx];
                if (c == d) {
                    rightmost_match_p[needle_idx] = i;
                    needle_idx--;
                }
            }
        }
        if (compute_bitmasks) {
            *haystack_bitmask = mask;
        }
        if (needle_idx != -1) {
            return 0.0;
        }

        // Prepare for memoization.
        // - Snip off corners.
        // - Valid because we know needle_len < haystack_len from above.
        // - Avoid collisions above with a guard clause.
        long haystack_limit = rightmost_match_p[m.needle_len - 1] + 1;
        long memo_size =
            haystack_limit * m.needle_len -
            (m.needle_len * m.needle_len - m.needle_len);
        double memo[memo_size];
        for (i = 0; i < memo_size; i++) {
            memo[i] = UNSET;
        }
        m.memo = memo;

        score = recursive_match(&m, 0, 0);
    }
    return score == NON_MATCH ? 0.0 : score;
}
