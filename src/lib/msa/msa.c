/* $Id: msa.c,v 1.55 2005-11-25 17:26:24 acs Exp $
   Written by Adam Siepel, 2002
   Copyright 2002, Adam Siepel, University of California 
*/

/** \file msa.c
   Multiple sequence alignments.
   \ingroup msa
*/

/*
   To do:

      - more general coordinate mapping?  Most functions use MSA
      coords, but some can convert using coordinate maps.

      - clean up indexing.  Usually 0-based indexing is used, but
      not always.

      - some obscure aspects of PHYLIP format not supported -- e.g.,
      problem when sequence name is not separated from sequence by
      whitespace.

      - should automatically recognize alignment format when reading.

      - unified handling of suff stats; they've become a special case
      that nearly every function has to consider.  Perhaps should just
      always do *everything* in terms of sufficient statistics, and
      eliminate explicit representation of alignments.
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <assert.h>

#include <lists.h>
#include <stacks.h>
#include <msa.h>
#include <misc.h>
#include <gff.h>
#include <category_map.h>
#include <hashtable.h>
#include <sufficient_stats.h>
#include <local_alignment.h>

/* whether to retain stop codons when cleaning an alignment of coding
   sequences; see msa_coding_clean */
#define KEEP_STOP_CODONS 0

/** Creates a new MSA object.  Two-dimensional character arrays must be
   passed in for sequences and names (no new memory is allocated for
   them).  The alphabet, however, will be copied into newly allocated
   memory.  If the "alphabet" argument is null, the default alphabet
   will be used. */
MSA *msa_new(char **seqs, char **names, int nseqs, int length, char *alphabet) {
  int i;
  MSA *msa = (MSA*)smalloc(sizeof(MSA));
  msa->seqs = seqs;
  msa->names = names;
  msa->nseqs = nseqs;
  msa->length = length;
  msa->categories = NULL;       /* init as needed */
/*   msa->base_freqs = NULL;  */      /* init as needed */
/*   msa->base_freqs_cat = -1; */
  msa->ss = NULL;               /* init as needed */
  msa->ncats = -1;
  msa->alloc_len = msa->length; /* assume alloc equals length */
  msa->idx_offset = 0;
  msa->is_informative = NULL;

  if (alphabet != NULL) {
    msa->alphabet = (char*)smalloc((strlen(alphabet) + 1) * sizeof(char));
    strcpy(msa->alphabet, alphabet);
  }
  else {
    msa->alphabet = (char*)smalloc((strlen(DEFAULT_ALPHABET)+1) * sizeof(char));
    strcpy(msa->alphabet, DEFAULT_ALPHABET);
  }
  msa->missing = DEFAULT_MDATA_CHARS;

  for (i = 0; i < NCHARS; i++) { 
    msa->inv_alphabet[i] = -1;
    msa->is_missing[i] = 0;
  }
  for (i = 0; msa->alphabet[i] != '\0'; i++)
    msa->inv_alphabet[(int)msa->alphabet[i]] = i;
  for (i = 0; msa->missing[i] != '\0'; i++)
    msa->is_missing[(int)msa->missing[i]] = 1;

  return msa;
}

/** Creates a new alignment from the contents of the specified file,
   which is assumed to use the specified format.  If "alphabet" is
   NULL, default alphabet for DNA will be used.  This routine will
   abort if the sequence contains a character not in the alphabet. */
MSA *msa_new_from_file(FILE *F, msa_format_type format, char *alphabet) {
  int i, j, k, nseqs, len, do_toupper;
  MSA *msa;
  String *tmpstr = str_new(STR_MED_LEN);

  if (format == FASTA) 
    return (msa_read_fasta(F, alphabet));
  else if (format == LAV)
    return la_to_msa(la_read_lav(F, 1), 0);
  else if (format == SS) 
    return ss_read(F, alphabet);

  if (format == PHYLIP || format == MPM) {
    if (fscanf(F, "%d %d", &nseqs, &len) <= 0) 
      die("ERROR: PHYLIP or MPM file missing initial length declaration.\n");
  }

  /* we'll initialize the MSA first, so that we can use its
   * "inv_alphabet" */
  msa = msa_new(NULL, NULL, nseqs, len, alphabet);
  msa->names = (char**)smalloc(nseqs * sizeof(char*));
  msa->seqs = (char**)smalloc(nseqs * sizeof(char*));

  /* upcase chars unless there are lowercase characters in the alphabet */
  do_toupper = !msa_alph_has_lowercase(msa);    

  for (i = 0; i < nseqs; i++) {
    msa->names[i] = (char*)smalloc(STR_MED_LEN * sizeof(char));
    msa->seqs[i] = (char*)smalloc((len + 1) * sizeof(char));
  }

  if (format == MPM) {
    for (i = 0; i < nseqs; i++) {
      do { str_readline(tmpstr, F); str_trim(tmpstr); } 
      while (tmpstr->length == 0);
      strcpy(msa->names[i], tmpstr->chars);
    }
  }
  for (i = 0; i < nseqs; i++) {
    char line[MAX_LINE_LEN];

    if (format == PHYLIP)
      fscanf(F, "%s", msa->names[i]); 
                                /* FIXME: this won't handle the weird
                                 * case in true PHYLIP format in which
                                 * the name is not separated from the
                                 * sequence by whitespace */ 
    else if (format == FASTA) {
      fgets(line, MAX_LINE_LEN, F);
      for (j = 0; line[j] != 0 && (line[j] == '>' || isspace(line[j])); j++);
      strcpy(msa->names[i], &line[j]);
    }

    j = 0;
    while (j < len) {
      fgets(line, MAX_LINE_LEN, F);
      for (k = 0; line[k] != '\0'; k++) {
        char base;
        if (isspace(line[k])) continue;
        base = do_toupper ? toupper(line[k]) : line[k];
        if (base == '.' && msa->inv_alphabet[(int)'.'] == -1) 
          base = msa->missing[0]; /* interpret '.' as missing data;
                                     maybe no longer necessary */
        else if (base != GAP_CHAR && !msa->is_missing[(int)base] &&
                 msa->inv_alphabet[(int)base] == -1) {
          if (isalpha(base)) base = 'N'; 
                                /* use 'N' in place of unrecognized
                                   alphabetical character */
          else 
            die("ERROR: bad character in multiple sequence alignment: '%c'.\n", 
                base); 
        }
        msa->seqs[i][j++] = base;
      }
    }
    /* should reach end of line and j=len simultaneously; otherwise
     * sequence is not advertised length */
    if (line[k] != '\0') 
      die("ERROR: bad sequence length in multiple alignment.\n"); 

    msa->seqs[i][j] = '\0';
  }
  str_free(tmpstr);

  return msa;
}

/** create a copy of an MSA.  If suff_stats_only == 1, then sequences
   aren't copied */
MSA *msa_create_copy(MSA *msa, int suff_stats_only) {
  char **new_names, **new_seqs;
  int i;
  MSA *retval;

  assert(!(suff_stats_only && msa->ss == NULL));

  /* copy names */
  new_names = smalloc(msa->nseqs * sizeof(char*));
  for (i = 0; i < msa->nseqs; i++) new_names[i] = strdup(msa->names[i]);

  /* copy seqs, if necessary */
  if (!suff_stats_only && msa->seqs != NULL) {
    new_seqs = smalloc(msa->nseqs * sizeof(char*));
    for (i = 0; i < msa->nseqs; i++) new_seqs[i] = strdup(msa->seqs[i]);
  }
  else new_seqs = NULL;

  retval = msa_new(new_seqs, new_names, msa->nseqs, msa->length, 
                   msa->alphabet);

  retval->ncats = msa->ncats;
  retval->idx_offset = msa->idx_offset;

  if (msa->categories != NULL) {
    retval->categories = smalloc(msa->length * sizeof(int));
    memcpy(retval->categories, msa->categories, msa->length * sizeof(int));
  }
/*   if (msa->base_freqs != NULL) { */
/*     retval->base_freqs = vec_new(msa->base_freqs->size); */
/*     vec_copy(retval->base_freqs, msa->base_freqs); */
/*     retval->base_freqs_cat = msa->base_freqs_cat; */
/*   } */

  if (msa->ss != NULL) 
    ss_from_msas(retval, msa->ss->tuple_size, (msa->ss->tuple_idx != NULL),
                 NULL, msa, NULL, -1); /* will be created from msa->ss */

  return retval;
}

/* kept separate for now */
MSA *msa_read_fasta(FILE *F, char *alphabet) {
  List *names = lst_new_ptr(10);
  List *seqs = lst_new_ptr(10);
  static Regex *descrip_re = NULL;
  int maxlen, i, nseqs, j, do_toupper;
  String *line = str_new(STR_MED_LEN);
  List *l = lst_new_ptr(2);
  String *new_str = NULL;
  MSA *msa;

  if (descrip_re == NULL) 
    descrip_re = str_re_new("[[:space:]]*>[[:space:]]*([^[:space:]]+)");

  while ((str_readline(line, F)) != EOF) {
    if (str_re_match(line, descrip_re, l, 1) > 0) {
      lst_push_ptr(names, lst_get_ptr(l, 1));
      str_free((String*)lst_get_ptr(l, 0));

      new_str = str_new(STR_MED_LEN);
      lst_push_ptr(seqs, new_str);
      continue;
    }

    str_double_trim(line);
    if (line->length == 0) continue;

    if (new_str == NULL) 
      die("ERROR in FASTA file: non-blank line preceding first description ('>') line.\n");

    str_append(new_str, line);
  }

  if (lst_size(seqs) == 0)
    die("ERROR: empty FASTA file.\n");

  /* pad sequences with gaps if not same length */
  maxlen = 0;
  for (i = 0; i < lst_size(seqs); i++)
    if (((String*)lst_get_ptr(seqs, i))->length > maxlen)
      maxlen = ((String*)lst_get_ptr(seqs, i))->length;

  for (i = 0; i < lst_size(seqs); i++) {
    String *s = (String*)lst_get_ptr(seqs, i);
    for (j = s->length; j < maxlen; j++)
      str_append_char(s, GAP_CHAR);
  }

  /* now create MSA */
  nseqs = lst_size(names);
  assert(nseqs == lst_size(seqs));

  msa = msa_new(NULL, NULL, nseqs, maxlen, alphabet);
  msa->names = (char**)smalloc(nseqs * sizeof(char*));
  msa->seqs = (char**)smalloc(nseqs * sizeof(char*));

  /* upcase chars unless there are lowercase characters in the alphabet */
  do_toupper = !msa_alph_has_lowercase(msa);    

  for (i = 0; i < nseqs; i++) {
    String *n, *s;
    n = (String*)lst_get_ptr(names, i);
    msa->names[i] = (char*)smalloc((n->length + 1) * sizeof(char));
    strcpy(msa->names[i], n->chars);
    str_free(n);

    s = (String*)lst_get_ptr(seqs, i);
    msa->seqs[i] = (char*)smalloc((maxlen + 1) * sizeof(char));

    /* scan chars and adjust if necessary */
    for (j = 0; j < maxlen; j++) {
      msa->seqs[i][j] = do_toupper ? toupper(s->chars[j]) : s->chars[j];
      if (msa->seqs[i][j] == '.' && msa->inv_alphabet[(int)'.'] == -1) 
        msa->seqs[i][j] = msa->missing[0]; /* interpret '.' as missing
                                              data; maybe no longer
                                              necessary */
      if (isalpha(msa->seqs[i][j]) && msa->inv_alphabet[(int)msa->seqs[i][j]] == -1)
        msa->seqs[i][j] = 'N'; /* assume 'N' if unrecognized letter */
    }
    msa->seqs[i][maxlen] = '\0';

    str_free(s);
  }  

  lst_free(names);
  lst_free(seqs);
  lst_free(l);
  str_free(line);

  return msa;
}

/** Prints MSA to file, using specified format.  The "pretty_print"
   option causes periods ('.') to be printed in place of characters
   that are identical to corresponding characters in the first
   sequence. */
void msa_print(FILE *F, MSA *msa, msa_format_type format, int pretty_print) {
  int i, j, k;
  if (format == SS) {
    if (msa->ss == NULL) ss_from_msas(msa, 1, 1, NULL, NULL, NULL, -1);
    ss_write(msa, F, 1);
    return;
  }

  /* otherwise, require explicit representation of alignment */
  if (msa->seqs == NULL && msa->ss != NULL) ss_to_msa(msa);

  if (format == PHYLIP || format == MPM)
    fprintf(F, "  %d %d\n", msa->nseqs, msa->length);
  if (format == MPM)
    for (i = 0; i < msa->nseqs; i++) 
      fprintf(F, "%s\n", msa->names[i]);
  for (i = 0; i < msa->nseqs; i++) {
    if (format == PHYLIP)
      fprintf(F, "%s\n", msa->names[i]);
    else if (format == FASTA)
      fprintf(F, "> %s\n", msa->names[i]);
    for (j = 0; j < msa->length; j += OUTPUT_LINE_LEN) {
      for (k = 0; k < OUTPUT_LINE_LEN && j + k < msa->length; k++) 
        if (pretty_print && i > 0 && msa->seqs[i][j+k] == msa->seqs[0][j+k])
          fprintf(F, ".");
        else
	  fprintf(F, "%c", msa->seqs[i][j+k]);
      if (format == PHYLIP || format == FASTA) fprintf(F, "\n");
    }
    if (format == MPM) fprintf(F, "\n");
  }
}

/* Frees MSA object.  Names and seqs are freed also, even though they may
   have been allocated externally. */
void msa_free(MSA *msa) {
  int i;
  for (i = 0; i < msa->nseqs; i++) {
    if (msa->names != NULL && msa->names[i] != NULL) 
      free(msa->names[i]);
    if (msa->seqs != NULL && msa->seqs[i] != NULL) 
      free(msa->seqs[i]);
  }
  if (msa->names != NULL) free(msa->names);
  if (msa->seqs != NULL) free(msa->seqs);
  if (msa->alphabet != NULL) free(msa->alphabet);
  if (msa->categories != NULL) free(msa->categories);
  if (msa->ss != NULL) ss_free(msa->ss);
  if (msa->is_informative != NULL) free(msa->is_informative);
  free(msa);
}

/** If gap_strip_mode is STRIP_ALL_GAPS or STRIP_ANY_GAPS, removes all
   columns with ALL or ANY gaps, respectively.  Otherwise, assumes a
   *projection* is desired onto the sequence whose index is
   gap_strip_mode (indexing starts with 1).  Changes are made to
   original alignment.  Gaps are expected to be represented by
   GAP_CHAR.  If msa->categories is non-NULL, will be adjusted
   accordingly. */
void msa_strip_gaps(MSA *msa, int gap_strip_mode) {
  int i, j, k, strip;
  char c;

  if (msa->seqs != NULL && msa->ss != NULL) { /* in this case, work
                                                 with seqs */
    ss_free(msa->ss);
    msa->ss = NULL;
  }

  if (msa->ss != NULL) {
    ss_strip_gaps(msa, gap_strip_mode);
    return;
  }

  if (gap_strip_mode > 0) { 
    msa_project(msa, gap_strip_mode);
    return;
  }

  assert(gap_strip_mode == STRIP_ALL_GAPS || gap_strip_mode == STRIP_ANY_GAPS);
  k = 0;
  for (i = 0; i < msa->length; i++) {
    strip = (gap_strip_mode == STRIP_ALL_GAPS);
    for (j = 0; j < msa->nseqs; j++) {
      c = msa->seqs[j][i];
      if (gap_strip_mode == STRIP_ANY_GAPS && c == GAP_CHAR) {
        strip = TRUE; break;
      }
      else if (gap_strip_mode == STRIP_ALL_GAPS && c != GAP_CHAR) {
        strip = FALSE; break;
      }
    }

    if (k == i && !strip) k++;
    else if (!strip) {
      for (j = 0; j < msa->nseqs; j++) 
        msa->seqs[j][k] = msa->seqs[j][i];
      if (msa->categories != NULL)
        msa->categories[k] = msa->categories[i];
      k++;
    }
  }
  msa->length = k;
}

/* "project" alignment on specified sequence, by eliminating all
   columns in which that sequence has a gap.  Indexing of sequences
   starts with 1 */
void msa_project(MSA *msa, int refseq) {
  int i, j, k;
  assert(refseq >= 1 && refseq <= msa->nseqs);
  k = 0;
  for (i = 0; i < msa->length; i++) {
    if (msa->seqs[refseq-1][i] != GAP_CHAR) {
      for (j = 0; k != i && j < msa->nseqs; j++)
        msa->seqs[j][k] = msa->seqs[j][i];
      if (msa->categories != NULL)
        msa->categories[k] = msa->categories[i];
      k++;      
    }
  }

  msa->length = k;
}

/* Returns a sub-alignment consisting of the specified sequences
   within the specified range of columns.  Listed sequence will be
   included if "include" == TRUE and excluded otherwise (include is
   ignored if seqlist == NULL).  In either case, indices, not names,
   must be used.  All memory is copied.  To include all sequences, set
   seqlist to NULL.  The new alignment will represent the interval
   [start_col, end_col), in a frame such that the first character has
   index 0.  (that is, the end column will not be included).  */
MSA* msa_sub_alignment(MSA *msa, List *seqlist, int include, int start_col, 
                       int end_col) {
  List *include_list;
  int i, j;
  MSA *new_msa;

  int new_nseqs;
  char **new_names;
  char **new_seqs;
  int new_len = end_col - start_col;

  assert(new_len > 0);
  assert(msa->seqs != NULL || msa->ss != NULL);

  if (seqlist != NULL)
    for (i = 0; i < lst_size(seqlist); i++)
      if (lst_get_int(seqlist, i) < 0 || lst_get_int(seqlist, i) >= msa->nseqs)
        die("ERROR: sequence index out of range in msa_sub_alignment.\n");

  /* if in "exclude" mode, find complement of indicated set */
  if (!include && seqlist != NULL) {
    int tmparray[msa->nseqs];
    include_list = lst_new_int(msa->nseqs);
    for (i = 0; i < msa->nseqs; i++) tmparray[i] = 1;
    for (i = 0; seqlist != NULL && i < lst_size(seqlist); i++) 
      tmparray[lst_get_int(seqlist, i)] = 0;
    for (i = 0; i < msa->nseqs; i++) 
      if (tmparray[i] == 1)
        lst_push_int(include_list, i);
  }
  else if (seqlist != NULL)     /* include == TRUE */
    include_list = seqlist; 
  else {                        /* seqlist == NULL: include everything */
    include_list = lst_new_int(msa->nseqs);
    for (i = 0; i < msa->nseqs; i++) lst_push_int(include_list, i);
  }

  new_nseqs = lst_size(include_list);
  new_names = (char**)smalloc(new_nseqs * sizeof(char*));

  /* copy names */
  for (i = 0; i < lst_size(include_list); i++) 
    new_names[i] = strdup(msa->names[lst_get_int(include_list, i)]);

  if (msa->seqs != NULL) {      /* have explicit sequences */
    /* copy seqs */
    new_seqs = (char**)smalloc(new_nseqs * sizeof(char*));
    for (i = 0; i < lst_size(include_list); i++) {
      int seq = lst_get_int(include_list, i);
      new_seqs[i] = (char*)smalloc((new_len + 1) * sizeof(char));
      for (j = 0; j < new_len; j++)
        new_seqs[i][j] = msa->seqs[seq][start_col+j];
    }
    
    new_msa = msa_new(new_seqs, new_names, new_nseqs, new_len, 
                      msa->alphabet);

    if (msa->ncats >= 0 && msa->categories != NULL) {
      new_msa->ncats = msa->ncats;
      new_msa->categories = (int*)smalloc(new_len * sizeof(int));
      for (j = 0; j < new_len; j++)
        new_msa->categories[j] = msa->categories[start_col+j];
    }
  }
  else                          /* have only sufficient statistics */
    new_msa = ss_sub_alignment(msa, new_names, include_list, start_col, 
                               end_col);

  if (include_list != seqlist)
    lst_free(include_list);

  new_msa->idx_offset = msa->idx_offset + start_col;

  return new_msa;
}


/* Builds a "coordinate map" object with respect to the designated
   sequence.  Indexing begins with 1. */
msa_coord_map* msa_build_coord_map(MSA *msa, int refseq) {

  int i, j, last_char_gap;
  msa_coord_map* map = (msa_coord_map*)smalloc(sizeof(msa_coord_map));

  assert(msa->seqs != NULL || msa->ss != NULL);

  map->msa_list = lst_new_int(msa->length/10 + 1);
  map->seq_list = lst_new_int(msa->length/10 + 1);
                                /* list library will
                                 * srealloc if necessary */
  map->msa_len = msa->length;

  j = 0;
  last_char_gap = 1;
  for (i = 0; i < msa->length; i++) {
    char c = (msa->seqs != NULL ? msa->seqs[refseq-1][i] : 
              ss_get_char_pos(msa, i, refseq-1, 0));
    if (c == GAP_CHAR) 
      last_char_gap = 1;
    else {
      if (last_char_gap) {
        lst_push_int(map->msa_list, i+1);
        lst_push_int(map->seq_list, j+1);
      }
      j++;
      last_char_gap = 0;
    }
  }
  map->seq_len = j; 
  return map;
}

/* dump coord map; useful for debugging */
void msa_coord_map_print(FILE *F, msa_coord_map *map) {
  int i;
  for (i = 0; i < lst_size(map->seq_list); i++)
    fprintf(F, "%d\t%d\t%d\n", lst_get_int(map->seq_list, i), lst_get_int(map->msa_list, i), i > 0 ? lst_get_int(map->msa_list, i) - lst_get_int(map->seq_list, i) - lst_get_int(map->msa_list, i-1) + lst_get_int(map->seq_list, i-1) : -1);
}

/* Using a specified coordinate map object, converts a sequence
   coordinate to an MSA coordinate.  Indexing begins with 1. 
   Returns -1 if sequence coordinate is out of bounds. */
int msa_map_seq_to_msa(msa_coord_map *map, int seq_pos) {
  int idx, prec_match_msa_pos, prec_match_seq_pos;
  if (seq_pos < 1 || seq_pos > map->seq_len) return -1;
  idx = lst_bsearch_int(map->seq_list, seq_pos);
  assert(idx >= 0 && idx < lst_size(map->msa_list));
  prec_match_msa_pos = lst_get_int(map->msa_list, idx);
  prec_match_seq_pos = lst_get_int(map->seq_list, idx);
  return (prec_match_msa_pos + (seq_pos - prec_match_seq_pos));
}

/* Using a specified coordinate map object, converts an MSA coordinate
   to a sequence coordinate.  Returns -1 if index is out of range.
   Indexing begins with 1. */
int msa_map_msa_to_seq(msa_coord_map *map, int msa_pos) {
  int idx, prec_match_msa_pos, prec_match_seq_pos, next_match_seq_pos, 
    seq_pos;
  if (msa_pos < 1 || msa_pos > map->msa_len) return -1;
  idx = lst_bsearch_int(map->msa_list, msa_pos);
  if (idx < 0) return -1;
  assert(idx < lst_size(map->msa_list));
  prec_match_msa_pos = lst_get_int(map->msa_list, idx);
  prec_match_seq_pos = lst_get_int(map->seq_list, idx);
  next_match_seq_pos = (idx < lst_size(map->seq_list) - 1 ? 
                        lst_get_int(map->seq_list, idx + 1) :
                        map->seq_len + 1);

  seq_pos = prec_match_seq_pos + (msa_pos - prec_match_msa_pos);

  /* check to see if coordinate falls in gapped region of sequence.
     If it does, return position immediately preceding the gap */
  if (seq_pos >= next_match_seq_pos) 
    seq_pos = next_match_seq_pos - 1;

  return (seq_pos);
}

/* Create an empty coordinate map, of the specified starting size */
msa_coord_map* msa_new_coord_map(int size) {
  msa_coord_map* map = (msa_coord_map*)smalloc(sizeof(msa_coord_map));
  map->msa_list = lst_new_int(size);
  map->seq_list = lst_new_int(size);
  map->msa_len = map->seq_len = -1;
  return map;
}

/* Frees a coordinate map object */
void msa_map_free(msa_coord_map *map) {
  lst_free(map->msa_list);
  lst_free(map->seq_list);
  free(map);
}

/* what to do with overlapping categories? for now, just rely on external code to order them appropriately ... */ 
/* TODO: document "MSA" convention; provide option to specify source sequence 
   explicitly */
/* warning: requires coordinates of GFF_Set to be in frame of ref of entire alignment */
void msa_label_categories(MSA *msa, GFF_Set *gff, CategoryMap *cm) {
  int cat, i, j;
  GFF_Feature *feat;

  if (msa->categories == NULL) 
    msa->categories = (int*)smalloc(msa->length * sizeof(int));
  msa->ncats = cm->ncats;

  /* begin by initializing all categories to "other" */
  for (i = 0; i < msa->length; i++) msa->categories[i] = 0;

  for (i = 0; i < lst_size(gff->features); i++) {
    feat = (GFF_Feature*)lst_get_ptr(gff->features, i);
    cat = cm_get_category(cm, feat->feature); 

    if (cat == 0 && !str_equals_charstr(feat->feature, BACKGD_CAT_NAME))
      continue;                 /* don't label in case of unrecognized
                                   feature */

    if (feat->start == -1 || feat->end == -1 || feat->end >= msa->length) {
      fprintf(stderr, "WARNING: ignoring out-of-range feature\n");
      gff_print_feat(stderr, feat);
      continue;
    }

    if (cm->ranges[cat]->start_cat_no == cm->ranges[cat]->end_cat_no) {
      for (j = feat->start; j <= feat->end; j++) {
        int oldprec = cm->labelling_precedence[msa->categories[j-1]];
        int newprec = cm->labelling_precedence[cat];
        if (oldprec == -1 || (newprec != -1 && newprec < oldprec))
          msa->categories[j-1] = cat;
      }
    }
    else {
      int range_size = cm->ranges[cat]->end_cat_no - 
        cm->ranges[cat]->start_cat_no + 1;
      int frm;

      if (feat->frame < 0 || feat->frame > 2)
        frm = 0;                /* FIXME: something better here? */
      else
        frm = feat->frame;

      for (j = feat->start; j <= feat->end; j++) {
        int offset = feat->strand == '-' ? feat->end - j : j - feat->start;
        int thiscat = cm->ranges[cat]->start_cat_no + 
          (offset + frm) % range_size;
        int oldprec = cm->labelling_precedence[msa->categories[j-1]];
        int thisprec = cm->labelling_precedence[thiscat];
        if (oldprec == -1 || (thisprec != -1 && thisprec < oldprec))
          msa->categories[j-1] = thiscat;
      }
    }
  }
  if (msa->ss != NULL) 
    ss_update_categories(msa);
}

/** Return sequence index of given sequence name or -1 if not found. */
int msa_get_seq_idx(MSA *msa, char *name) {
  int i, retval = -1;
  for (i = 0; retval < 0 && i < msa->nseqs; i++) 
    if (!strcmp(name, msa->names[i]))
      retval = i;
  return retval;
}

/* converts coordinates of all features in a GFF_Set from one frame of
   reference to another.  Arguments from and to may be an index
   between 1 and nseqs, or 0 (for the frame of the entire alignment).
   If argument from is -1, the from index is inferred feature by
   feature by sequence name. Features whose start and end coords are
   out of range will be dropped; if only the start or the end is out
   of range, they will be truncated. If cm is non-NULL, features
   within groups will be forced to be contiguous. */
void msa_map_gff_coords(MSA *msa, GFF_Set *gff, int from_seq, int to_seq, 
                        int offset, CategoryMap *cm) {

  msa_coord_map **maps;
  int fseq = from_seq;
  int tseq = to_seq;
  String *prev_name = NULL;
  msa_coord_map *from_map = NULL, *to_map = NULL;
  GFF_Feature *feat;
  int i, s, e, orig_span;
  List *keepers = lst_new_ptr(lst_size(gff->features));

  maps = (msa_coord_map**)smalloc((msa->nseqs + 1) * 
                                  sizeof(msa_coord_map*));

  for (i = 0; i <= msa->nseqs; i++) maps[i] = NULL;

  for (i = 0; i < lst_size(gff->features); i++) {
    feat = (GFF_Feature*)lst_get_ptr(gff->features, i);

    if (from_seq == to_seq) {
      feat->start += offset;
      feat->end += offset;
      continue;
    }

    else if (from_seq == -1) {
      if (str_equals_nocase_charstr(feat->seqname, "MSA")) 
        fseq = 0;
      else if (prev_name == NULL || !str_equals(prev_name, feat->seqname)) {
        /* generally all seqs will have the same name; take advantage of
           this property */
        if ((fseq = msa_get_seq_idx(msa, feat->seqname->chars)) == -1) 
          die("ERROR: name %s not present in MSA.\n", feat->seqname->chars);
        fseq++;                 /* need 1-based index */
        prev_name = feat->seqname;
      }
    }
    else if (to_seq == -1) {
      if (str_equals_nocase_charstr(feat->seqname, "MSA")) 
        tseq = 0;
      else if (prev_name == NULL || !str_equals(prev_name, feat->seqname)) {
        if ((tseq = msa_get_seq_idx(msa, feat->seqname->chars)) == -1)
          die("ERROR: name %s not present in MSA.\n", feat->seqname->chars);
        prev_name = feat->seqname;
        tseq++;                 /* need 1-based index */
      }
    }
    if ((from_map = maps[fseq]) == NULL && fseq > 0) 
      from_map = maps[fseq] = msa_build_coord_map(msa, fseq);

    if ((to_map = maps[tseq]) == NULL && tseq > 0) 
      to_map = maps[tseq] = msa_build_coord_map(msa, tseq);

    orig_span = feat->end - feat->start;

    /* from_map, to_map will be NULL iff fseq, to_seq are 0 */
    s = msa_map_seq_to_seq(from_map, to_map, feat->start);
    e = msa_map_seq_to_seq(from_map, to_map, feat->end);

    if (s < 0 && e < 0) {
      gff_free_feature(feat);
      continue;
    }

    feat->start = (s < 0 ? 1 : s) + offset;

    if (e < 0)
      feat->end = (to_map != NULL ? to_map->seq_len : msa->length) + offset;
    else
      feat->end = e + offset;

    lst_push_ptr(keepers, feat);

    /* TEMPORARY: Prevent overall size of "signal" (non-cyclic)
       features from changing.  This needs to be redone in a general
       way, e.g., using a def. in the cm of cyclic versus non-cyclic
       categories, and a definition of an "anchor" site for non-cyclic
       ones (acs, 1/04) */
    if (feat->end - feat->start != orig_span) {
      int lanchor = FALSE, ranchor = FALSE;

      /* left-anchored */
      if (str_equals_charstr(feat->feature, "5'splice") || 
          str_equals_charstr(feat->feature, "start_codon") || 
          str_equals_charstr(feat->feature, "stop_codon") || 
          str_equals_charstr(feat->feature, "cds3'ss")) 
        lanchor = TRUE;
      /* right-anchored */
      else if (str_equals_charstr(feat->feature, "3'splice") || 
               str_equals_charstr(feat->feature, "cds5'ss") ||
               str_equals_charstr(feat->feature, "prestart"))
        ranchor = TRUE;

      if ((lanchor && feat->strand == '+') || (ranchor && feat->strand == '-'))
        feat->end = feat->start + orig_span;
      else if ((ranchor && feat->strand == '+') || (lanchor && feat->strand == '-'))
        feat->start = feat->end - orig_span;
    }

    /* NOTE: fill precedence stuff now removed -- should take out of
       category_map.c */
  }

  lst_free(gff->features);
  gff->features = keepers;
  if (gff->groups != NULL) gff_ungroup(gff);

  for (i = 1; i <= msa->nseqs; i++)
    if (maps[i] != NULL) msa_map_free(maps[i]);
  free(maps);
}

/* for convenience when going from one sequence to another.  use map=NULL to
   indicate frame of entire alignment.
   Returns -1 if out of range.
*/
int msa_map_seq_to_seq(msa_coord_map *from_map, msa_coord_map *to_map, 
                       int coord) { 
  int msa_coord = (from_map == NULL ? coord : 
    msa_map_seq_to_msa(from_map, coord));
  if (msa_coord == -1) return -1;
  return (to_map == NULL ? msa_coord : msa_map_msa_to_seq(to_map, msa_coord));
}

char msa_compl_char(char c) {
  if (c == 'A') return 'T';
  else if (c == 'C') return 'G';
  else if (c == 'G') return 'C';
  else if (c == 'T') return 'A';
  return c;
}

void msa_reverse_compl_seq(char *seq, int length) {
  int i, midpt;
  if (length <= 0) return;
  midpt = (length-1)/2;
  for (i = 0; i <= midpt; i++) {
    char tmp = msa_compl_char(seq[i]);
    seq[i] = msa_compl_char(seq[length-i-1]);
    seq[length-i-1] = tmp;
                                /* NOTE: case of middle base in
                                   odd-length seq is a little
                                   subtle */
  }
}

/* Similar to above, but for a segment of a sequence.  Note: start and
   end both inclusive and using indexing system that begins with 1
   (*not* the system used for storage). */
void msa_reverse_compl_seq_segment(char *seq, int start, int end) {
  int i, midpt;
  start--; end--;               /* switch to indexing system used for storage */
  if (end < start) return;
  midpt = start + (end-start)/2; 
  for (i = start; i <= midpt; i++) {
    char tmp = msa_compl_char(seq[i]);
    seq[i] = msa_compl_char(seq[end-i+start]);
    seq[end-i+start] = tmp;
                                /* NOTE: case of middle base in
                                   odd-length segment is a little
                                   subtle */
  }
}

/* Same as above, but for an auxiliary array of integers */
void msa_reverse_data_segment(int *data, int start, int end) {
  int i, midpt;
  start--; end--; 
  if (end < start) return;
  midpt = start + (end-start)/2; 
  for (i = start; i <= midpt; i++) {
    int tmp = data[i];
    data[i] = data[end-i+start];
    data[end-i+start] = tmp;
  }
}

/* Reverse complement an entire alignment. */
void msa_reverse_compl(MSA *msa) {
  int i, tupsize = -1, store_order = 0;

  if (msa->ss == NULL) assert(msa->categories == NULL);
                                /* FIXME: ss case is handling
                                   categories but other case isn't */
  
  /* temporary -- work-around for problem with context being wrong
     in suff stats at boundaries of MAF blocks; reverse complement
     using the complete alignment, not just the suff stats */
  if (msa->ss != NULL && msa->ss->tuple_size > 1) {
    tupsize = msa->ss->tuple_size;
    store_order = (msa->ss->tuple_idx != NULL);
    if (msa->seqs == NULL) ss_to_msa(msa);
    ss_free(msa->ss);
    msa->ss = NULL;
  }
  /* end temporary */

  if (msa->seqs != NULL) 
    for (i = 0; i < msa->nseqs; i++) 
      msa_reverse_compl_seq(msa->seqs[i], msa->length);

  if (msa->ss != NULL) 
    ss_reverse_compl(msa);

  /* temporary (recreate SS)  FIXME: check */
  if (tupsize != -1) 
    ss_from_msas(msa, tupsize, store_order, NULL, NULL, NULL, -1);      
  /* end temporary */
}

/* Reverse complement a segment of an alignment.  Note: start and end
   both inclusive and using indexing system that begins with 1 (*not*
   the system used for storage). */
void msa_reverse_compl_segment(MSA *msa, int start, int end) {
  int i;
  assert(msa->ss == NULL);      /* suff stats not yet supported */
  for (i = 0; i < msa->nseqs; i++) 
    msa_reverse_compl_seq_segment(msa->seqs[i], start, end);
}

/** Reverse complement segments of an MSA corresponding to groups of
   features on the reverse strand.  Adjusts the coordinates in the
   GFF_Set accordingly.  This function can be used to ensure that
   sites in strand-specific categories (e.g., 1st codon position,
   intron) are oriented consistently.  It can be useful in phylogenetic
   analysis and in training the transition probabilities of a phylo-HMM.
   Features are assumed already to have been grouped as desired, and
   groups are assumed to be nonoverlapping (see gff_group and
   gff_remove_overlaps).  Strandedness is tested using
   gff_reverse_strand_only.  The GFF_Set is assumed to use the
   coordinate frame of the alignment.  */
void msa_reverse_compl_feats(MSA *msa, 
                                /**< Alignment object.  If NULL, only
                                   the GFF_Set (and optionally
                                   aux_data) will be altered */
                             GFF_Set *feats, 
                                /**< Set of features */
                             int *aux_data
                                /**< Auxiliary array of site-specific
                                   integers (e.g., gap patterns) to be
                                   kept in sync with the alignment
                                   and/or GFF_Set */
                             ) {
  int i;

  if (lst_size(feats->features) == 0) return;

  if (msa != NULL) assert(msa->ss == NULL);      
                                /* not yet equipped to handle suff stats */

  if (feats->groups == NULL) 
    die("ERROR: msa_reverse_compl_feats requires grouped features.\n");

  for (i = 0; i < lst_size(feats->groups); i++) {
    GFF_FeatureGroup *g = lst_get_ptr(feats->groups, i);
    if (gff_reverse_strand_only(g->features)) {
      gff_reverse_compl(g->features, g->start, g->end);
      if (msa != NULL) {
        msa_reverse_compl_segment(msa, g->start, g->end);
        if (msa->categories != NULL)
          msa_reverse_data_segment(msa->categories, g->start, g->end);
      }
      if (aux_data != NULL) 
        msa_reverse_data_segment(aux_data, g->start, g->end);
    }
  }
}

/* tuple_size-1 columns of "missing data" characters (Ns) will be
   inserted between columns that were not adjacent in the original
   alignment (only has an effect when tuple_size > 1).  Use cats_to_do
   to specify categories explicitly (if NULL, all cats processed) */
void msa_partition_by_category(MSA *msa, List *submsas, List *cats_to_do, 
                               int tuple_size) {
  int i, j, cat, col, ncats = 1;
  int *count, *idx, *do_cat;
  char ***seqs, ***names;
  List *cats;

  /* scan for max category */ 
  for (i = 0; i < msa->length; i++)
    if (msa->categories[i] + 1 > ncats) 
      ncats = msa->categories[i] + 1;

  if (cats_to_do == NULL) {
    cats = lst_new_int(ncats);
    for (i = 0; i < ncats; i++) lst_push_int(cats, i);
  }
  else 
    cats = cats_to_do;

  do_cat = (int*)smalloc(ncats * sizeof(int));
  for (i = 0; i < ncats; i++) do_cat[i] = 0;
  for (i = 0; i < lst_size(cats); i++) 
    do_cat[lst_get_int(cats, i)] = 1;

  /* obtain counts for each category */
  count = (int*)smalloc(ncats * sizeof(int));
  for (i = 0; i < ncats; i++) count[i] = 0;
  for (i = 0; i < msa->length; i++) {
    assert(msa->categories[i] < ncats);
    count[msa->categories[i]]++;
    if (i > 0 && msa->categories[i] != msa->categories[i-1])
      count[msa->categories[i]] += tuple_size - 1;      
  }

  /* alloc seqs of appropriate size */
  seqs = (char***)smalloc(ncats * sizeof(char**));
  /* three dimensional array: first dimension is partition, second is
     index of sequence in alignment, third is column */
  for (i = 0; i < ncats; i++) {
    if (!do_cat[i]) { seqs[i] = NULL; continue; }
    seqs[i] = (char**)smalloc(msa->nseqs * sizeof(char*));
    for (j = 0; j < msa->nseqs; j++)
      seqs[i][j] = (char*)smalloc((count[i]+1) * sizeof(char*));
  }

  /* set up record of terminus of each alignment */
  idx = (int*)smalloc(ncats * sizeof(int));
  for (i = 0; i < ncats; i++) idx[i] = 0;

  /* copy sites to subalignments */
  for (j = 0; j < msa->length; j++) {
    cat = msa->categories[j];
    if (!do_cat[cat]) continue;
    if (j > 0 && cat != msa->categories[j-1] && idx[cat] > 0) {
      /* add columns of missing data, if necessary */
      for (col = 0; col < tuple_size-1; col++) {
        for (i = 0; i < msa->nseqs; i++) 
          seqs[cat][i][idx[cat]] = msa->missing[0];
        idx[cat]++;
      }
    }
    /* copy column */
    for (i = 0; i < msa->nseqs; i++) 
      seqs[cat][i][idx[cat]] = msa->seqs[i][j];
    idx[cat]++;
  }
  for (cat = 0; cat < ncats; cat++) 
    if (do_cat[cat])
      for (i = 0; i < msa->nseqs; i++) 
        seqs[cat][i][idx[cat]] = '\0';

  /* make a copy of the sequence names for each subalignment */
  names = (char***)smalloc(ncats * sizeof(char**));
  for (i = 0; i < ncats; i++) {
    if (!do_cat[i]) { names[i] = NULL; continue; }
    names[i] = (char**)smalloc(msa->nseqs * sizeof(char*));
    for (j = 0; j < msa->nseqs; j++) {
      names[i][j] = (char*)smalloc((strlen(msa->names[j]) + 1) * sizeof(char));
      strcpy(names[i][j], msa->names[j]);
    }
  }

  for (cat = 0; cat < ncats; cat++) {
    if (do_cat[cat]) {
      msa = msa_new(seqs[cat], names[cat], msa->nseqs, idx[cat], msa->alphabet);
      lst_push_ptr(submsas, msa);
    }
  }

  free(seqs);
  free(names);
  free(count);
  free(idx);
  free(do_cat);
  if (cats_to_do == NULL) lst_free(cats);
}

/* prints a single line of summary statistics for alignment: base
   freqs followed by total number of columns followed by number of
   columns containing any gaps followed by number of columns
   containing all gaps; if header == 1 prints header line instead of
   MSA summary (all following lines must describe MSAs having the same
   alphabet).  If start and end are *not* equal to -1, prints stats
   only for indicated interval (half-open, 0-based) */  
void msa_print_stats(MSA *msa, FILE *F, char *label, int header, int start, 
                     int end) {
  if (header == 1) {
    int i;
    fprintf(F, "%-20s ", "descrip.");
    for (i = 0; i < strlen(msa->alphabet); i++) 
        fprintf(F, "%10c ", msa->alphabet[i]);
    fprintf(F, "%10s ", "G+C");
    fprintf(F, "%10s ", "length");
    fprintf(F, "%10s ", "all_gaps");
    fprintf(F, "%10s\n", "some_gaps");
  }
  else {
    Vector *freqs = msa_get_base_freqs(msa, start, end);
    int nallgaps = msa_num_gapped_cols(msa, STRIP_ALL_GAPS, start, end);
    int nanygaps = msa_num_gapped_cols(msa, STRIP_ANY_GAPS, start, end);
    int i;
    double gc = 0;
    fprintf(F, "%-20s ", label);
    for (i = 0; i < strlen(msa->alphabet); i++) {
      fprintf(F, "%10.4f ", vec_get(freqs, i));
      if (msa->alphabet[i] == 'G' || msa->alphabet[i] == 'C')
        gc += vec_get(freqs, i);
    }
    fprintf(F, "%10.4f ", gc);
    fprintf(F, "%10u ", start >= 0 && end >= 0 ? end - start : msa->length);
    fprintf(F, "%10d ", nallgaps);
    fprintf(F, "%10d\n", nanygaps);
  }
}

/* Returns a (newly allocated) vector of size strlen(alphabet),
   consisting of frequencies listed in the order of the alphabet. If
   start and end are *not* -1, freqs are based on the indicated interval
   (half-open, 0-based) */
Vector *msa_get_base_freqs(MSA *msa, int start, int end) {
  int i, j, size = strlen(msa->alphabet);
  double sum = 0;
  int s = start > 0 ? start : 0, e = end > 0 ? end : msa->length;
  Vector *base_freqs = vec_new(size);
  vec_zero(base_freqs);

  if (msa->ss != NULL && (start != -1 || end != -1)) 
    assert(msa->ss->tuple_idx != NULL);

  /* use sufficient stats, if available; WARNING: considers only
     right-most column if tuple_size > 1 (possible problem if only
     subset of columns are represented) */
  if (msa->ss != NULL && start == -1 && end == -1) {
    for (i = 0; i < msa->ss->ntuples; i++) {
      for (j = 0; j < msa->nseqs; j++) {
        char c = ss_get_char_tuple(msa, i, j, 0);
        if (c != GAP_CHAR && !msa->is_missing[(int)c]) {
          int idx = msa->inv_alphabet[(int)c];
          if (idx == -1) 
            die("ERROR: unrecognized character in alignment ('%c').\n", c);
          vec_set(base_freqs, idx, 
                         vec_get(base_freqs, idx) + 
                         msa->ss->counts[i]); 
          sum += msa->ss->counts[i];
        }
      }
    }
  }

  else {
    for (i = s; i < e; i++) {
      for (j = 0; j < msa->nseqs; j++) {
        char c = msa_get_char(msa, j, i);
        if (c != GAP_CHAR && !msa->is_missing[(int)c]) {
          int idx = msa->inv_alphabet[(int)c];
          if (idx == -1) {
            fprintf(stderr, "ERROR: unrecognized character in alignment ('%c').\n", c);
            exit(1);
          }
          vec_set(base_freqs, idx, 
                         vec_get(base_freqs, idx) + 1); 
          sum++;
        }
      }
    }
  }

  if (sum == 0) vec_zero(base_freqs);
  else vec_scale(base_freqs, 1.0/sum);
  return base_freqs;
}

/* similar to above function but returns the frequencies of k-tuples
   of bases, rather than of individual bases.  A gap anywhere in a
   k-tuple causes it to be ignored.  The "cat" argument causes only
   bases of the specified category to be considered; use -1 for all
   categories.  By convention, it is the *last* character in each
   tuple whose category is considered (this convention makes sense for
   models that consider the *predecessors* of each base).  This
   function supports use of the sufficient statistics representation
   of an alignment, but requires that the tuple size equal k */
void msa_get_base_freqs_tuples(MSA *msa, Vector *freqs, int k, int cat) {
  double sum = 0;               /* better to use double than int (or
                                   long int) because of overflow */
  int i, j, ignore, tup_idx, l, alph_idx;
  int alph_size = strlen(msa->alphabet);
  vec_zero(freqs);

  /* use sufficient stats, if available */
  if (msa->ss != NULL) {
    assert(msa->ss->tuple_size == k); 
    assert(cat < 0 || (msa->ncats >= cat && msa->ss->cat_counts != NULL));
    for (i = 0; i < msa->ss->ntuples; i++) {
      for (j = 0; j < msa->nseqs; j++) {
        int offset;
        ignore = 0;
        tup_idx = 0;
        for (offset = -1*(k-1); !ignore && offset <= 0; offset++) {
          char c = ss_get_char_tuple(msa, i, j, offset);
          if ((alph_idx = msa->inv_alphabet[(int)c]) == -1)
            ignore = 1;
          else 
            tup_idx += alph_idx * int_pow(alph_size, -offset); 
        }
        if (!ignore) {
          int thiscount = (cat >= 0 ? msa->ss->cat_counts[cat][i] :
                           msa->ss->counts[i]);
          vec_set(freqs, tup_idx, 
                         vec_get(freqs, tup_idx) + 
                         thiscount); 
        }
      }
    }
  }

  else {
    assert(cat < 0 || msa->categories != NULL);
    for (i = 0; i < msa->length-k+1; i++) {
      if (cat != -1 && msa->categories != NULL && 
          msa->categories[i+k-1] != cat) 
        continue;
      for (j = 0; j < msa->nseqs; j++) {
        ignore = 0;
        tup_idx = 0;
        for (l = 0; !ignore && l < k; l++) {
          char c = msa->seqs[j][i+l];
          if ((alph_idx = msa->inv_alphabet[(int)c]) == -1)
            ignore = 1;
          else 
            tup_idx += alph_idx * int_pow(alph_size, (k-l)-1); 
        }
        if (!ignore) {
          vec_set(freqs, tup_idx, 
                         vec_get(freqs, tup_idx) + 1); 
        }
      }
    }
  }

  for (i = 0; i < freqs->size; i++) sum += vec_get(freqs, i);
  vec_scale(freqs, 1.0/sum);
}

/* return number of gapped columns.  If mode == STRIP_ANY_GAPS, a
   gapped column is one containing at least one gap; if mode ==
   STRIP_ALL_GAPS, a gapped column is one containing only gaps */
int msa_num_gapped_cols(MSA *msa, int gap_strip_mode, int start, int end) {
  int i, j, k = 0, has_gap;
  int s = start > 0 ? start : 0, e = end > 0 ? end : msa->length;

  assert(gap_strip_mode == STRIP_ALL_GAPS || gap_strip_mode == STRIP_ANY_GAPS);

  if (msa->ss != NULL && (start != -1 || end != -1)) 
    assert(msa->ss->tuple_idx != NULL);

  if (msa->ss != NULL && start == -1 && end == -1) {
    for (i = 0; i < msa->ss->ntuples; i++) {
      has_gap = (gap_strip_mode == STRIP_ALL_GAPS ? 1 : 0);
      for (j = 0; j < msa->nseqs; j++) {
        char c = ss_get_char_tuple(msa, i, j, 0);
        if (gap_strip_mode == STRIP_ANY_GAPS && c == GAP_CHAR) {
          has_gap = 1; break;
        }
        else if (gap_strip_mode == STRIP_ALL_GAPS && c != GAP_CHAR) {
          has_gap = 0; break;
        }
      }
      if (has_gap) k += msa->ss->counts[i];
    }
  }

  else {
    for (i = s; i < e; i++) {
      has_gap = (gap_strip_mode == STRIP_ALL_GAPS ? 1 : 0);
      for (j = 0; j < msa->nseqs; j++) {
        if (gap_strip_mode == STRIP_ANY_GAPS && 
            msa_get_char(msa, j, i) == GAP_CHAR) {
          has_gap = 1; break;
        }
        else if (gap_strip_mode == STRIP_ALL_GAPS && 
                 msa_get_char(msa, j, i) != GAP_CHAR) {
          has_gap = 0; break;
        }
      }
      if (has_gap) k++;
    }
  }

  return k;
}

/* return number of columns of specified category that are
   "informative" in the sense that they contain at least two non-gaps
   (or non-missing chars).  If cat == -1, all columns will be
   considered */
unsigned int msa_ninformative_sites(MSA *msa, int cat) {
  unsigned int retval = 0;
  int i, j;
  if (msa->ss != NULL) {
    for (i = 0; i < msa->ss->ntuples; i++) {
      int ninf = 0;
      for (j = 0; j < msa->nseqs; j++) {
        if (ss_get_char_tuple(msa, i, j, 0) != GAP_CHAR && 
            !msa->is_missing[(int)ss_get_char_tuple(msa, i, j, 0)]) {
          ninf++;
          if (ninf >= 2) {
            retval += (cat >= 0 ? msa->ss->cat_counts[cat][i] :
                       msa->ss->counts[i]);
            break;
          }
        }
      }
    }
  }
  else {
    for (i = 0; i < msa->length; i++) {
      int ninf = 0;
      if (cat >= 0 && msa->categories[i] != cat) continue;
      for (j = 0; j < msa->nseqs; j++) {
        if (msa->seqs[j][i] != GAP_CHAR && 
            !msa->is_missing[(int)msa->seqs[j][i]]) ninf++;
        if (ninf >= 2) {
          retval++;
          break;
        }
      }
    }
  }
  return retval;
}

/* read and return a single sequence from a FASTA file */
String *msa_read_seq_fasta(FILE *F) {
  static Regex *descrip_re = NULL;
  String *line = str_new(STR_MED_LEN);
  String *seq = NULL;

  if (descrip_re == NULL) 
    descrip_re = str_re_new("^[[:space:]]*>");

  while ((str_readline(line, F)) != EOF) {
    if (str_re_match(line, descrip_re, NULL, 0) > 0) {
      if (seq != NULL) return seq;
      seq = str_new(STR_LONG_LEN);
      continue;
    }

    str_double_trim(line);
    if (line->length == 0) continue;

    if (seq == NULL) 
      die("ERROR in FASTA file: non-blank line preceding first description ('>') line.\n");

    str_append(seq, line);
  }

  return seq;
}

/* macros for use in msa_coding_clean; see below */
#define IS_START(seq, i) ( toupper(seq[i]) == 'A' && toupper(seq[i+1]) == 'T' && toupper(seq[i+2]) == 'G' )
#define IS_STOP(seq, i) ( toupper(seq[i]) == 'T' && ((toupper(seq[i+1]) == 'A' && (toupper(seq[i+2]) == 'A' || toupper(seq[i+2]) == 'G')) || (toupper(seq[i+1]) == 'G' && toupper(seq[i+2]) == 'A')) )

/* Clean an alignment of coding sequences (CDS exons from genomic DNA
   or mRNAs).  Remove sites with gaps and short blocks of ungapped
   sites, also look for frame shifts.  The parameter 'refseq; should
   indicate a sequence known to be a CDS and to start with a start
   codon and end with a stop codon.  The parameter min_ncodons should
   indicate the minimum number of ungapped codons allowed.  The
   function returns 0 on success and 1 if the alignment is rejected
   entirely. */
/* TODO: add special handling of short, incomplete seqs (don't treat
   missing seq as gaps) */
/* NOTE: KEEP_STOP_CODONS (see top of file) now determines whether
   stop codons are retained */
int msa_coding_clean(MSA *msa, int refseq, int min_ncodons, 
                     String *errstr) {
  List *block_begs = lst_new_int(10);
  List *block_ends = lst_new_int(10);
  char *ref = msa->seqs[refseq]; /* for convenience below */
  int i, j, k, l, beg, end, blk_beg, blk_end, blk_size, frame, pos;
  int ngaps[msa->nseqs];
  int retval = 0, trunc = 0;
  char tmp_codon[3];

  /* find start and stop in ref seq; have to allow for the possibility
     that they contain gaps */
  for (pos = 0, i = 0; pos < 3; pos++) {
    for (; i < msa->length && ref[i] == GAP_CHAR; i++);
    if (i == msa->length) break;
    if (pos == 0) beg = i;
    tmp_codon[pos] = ref[i++];
  }    
  if (i == msa->length || ! IS_START(tmp_codon, 0)) {
    str_append_charstr(errstr, "Reference sequence does not begin with start codon.  ");
    retval = 1;
  }
  i = msa->length - 1;
  for (pos = 2; pos >= (KEEP_STOP_CODONS ? 0 : -1); pos--) {
    for (; i > beg && ref[i] == GAP_CHAR; i--);
    if (i == beg) break;
    if ((pos == 2 && KEEP_STOP_CODONS) || pos == -1)  
      end = i;
    if (pos >= 0) tmp_codon[pos] = ref[i--];
  }
  if (i == beg || ! IS_STOP(tmp_codon, 0)) {
    str_append_charstr(errstr, "Reference sequence does not end with stop codon.");
    retval = 1;
  }

  /* find beg and end of each gapless block of size at least
     min_ncodons */
  for (j = 0; j < msa->nseqs; j++) ngaps[j] = 0;
  i = beg;                      
  frame = 0;
  while (i <= end && retval != 1) {
    /* find next gapless column of codons, maintaining frame wrt
       reference seq; simultaneously keep track of the number of gaps
       encountered in each sequence (wrt the ref seq) */
    int gapless_codon_col = 1;  /* whether currently considered column
                                   of codons is gapless (so far) */
    assert(frame == 0 || ref[i] == GAP_CHAR); /* see incr of frame, below */
    while (i <= end) {
      for (j = 0; j < msa->nseqs; j++) {
        if (msa->seqs[j][i] == GAP_CHAR) {
          ngaps[j]++;
          gapless_codon_col = 0;
        }
      }
      if (gapless_codon_col == 1 && frame == 2)
        break;                  /* if true, we must have encountered
                                   three consecutive gapless columns,
                                   in-frame */
      i++;
      if (ref[i] != GAP_CHAR) {
        frame++;
        if (frame == 3) { frame = 0; gapless_codon_col = 1; }
      }
    }

    if (i > end) break;
    assert(frame == 2);
    blk_beg = i-2;

    /* find next col with gap */
    for (i++; i <= end; i++) {
      for (j = 0; j < msa->nseqs && msa->seqs[j][i] != GAP_CHAR; j++);
      if (j != msa->nseqs) break;
    }
    blk_size = (i - blk_beg)/3;
    blk_end = blk_beg + blk_size*3 - 1; /* ensures block in frame */
    i = blk_end + 1; 
    frame = (ref[i] != GAP_CHAR ? 0 : 2); /* careful -- new frame not
                                             necessarily 0 */

    if (blk_size >= min_ncodons) {
      /* if block starts with start codon, must have start codon in
         all seqs */
      if (blk_beg == beg) {
        for (j = 0; j < msa->nseqs && IS_START(msa->seqs[j], blk_beg); j++);
        if (j != msa->nseqs) continue;
      }
      /* similarly for stop codons */
      if (blk_end == end && KEEP_STOP_CODONS) {
        for (j = 0; j < msa->nseqs && IS_STOP(msa->seqs[j], blk_end-2); j++);
        if (j != msa->nseqs) continue;
                                /* FIXME: if block sufficiently large,
                                   and stop codon is bad, just snip it
                                   off? */
      }

      /* if this is not the first retained block, then all seqs must
         be frame-consistent with the previous retained block */
      if (lst_size(block_begs) > 0) {
        for (j = 0; j < msa->nseqs && ngaps[j] % 3 == ngaps[refseq] % 3; j++);
        if (j != msa->nseqs) {  /* if test fails, reject alignment */
          trunc = lst_get_int(block_ends, lst_size(block_ends) - 1) + 1;
                                /* use end of prev block */
        }
      }

      /* finally, check all seqs for in-frame stop codons */
      for (j = blk_beg; j <= blk_end - 2 && trunc == 0; j += 3) {
        if (KEEP_STOP_CODONS && j == end - 2) break;
        for (k = 0; k < msa->nseqs && trunc == 0; k++) {
          if (IS_STOP(msa->seqs[k], j)) {
            trunc = blk_end = (KEEP_STOP_CODONS ? j + 2 : j - 1);
          }
        }
      }

      if (trunc == 0 || trunc > blk_beg) {
        lst_push_int(block_begs, blk_beg);
        lst_push_int(block_ends, blk_end);
      }

      if (trunc != 0) break;

      /* reset ngaps iff the block is retained */
      for (j = 0; j < msa->nseqs; j++) ngaps[j] = 0;
    }
  }

  if (retval != 1 && lst_size(block_begs) == 0) {
    str_append_charstr(errstr, "Nothing left after cleaning.");
    retval = 1;
  }

  /* reject the alignment if an in-frame stop or frame shift was found
     and it was not in the last 20% (otherwise the upstream portion
     will be retained).  */
  if (trunc != 0 && trunc < beg + (end - beg + 1) * 0.8) {  
    str_append_charstr(errstr, "In-frame stop codon or frame shift not in last 20% of alignment.  See approx. position ");
    str_append_int(errstr, trunc+1); /* report a 1-based index */
    str_append_charstr(errstr, ".");
    retval = 1;
  }

  /* now overwrite alignment with just the acceptable blocks */
  if (retval != 1) {
    for (i = 0, j = 0; j < lst_size(block_begs); j++) {
      blk_beg = lst_get_int(block_begs, j);
      blk_end = lst_get_int(block_ends, j);
      assert((blk_end - blk_beg + 1) % 3 == 0);
      for (k = blk_beg; k <= blk_end; k++) {
        for (l = 0; l < msa->nseqs; l++)
          msa->seqs[l][i] = msa->seqs[l][k];
        i++;
      }
    }
    msa->length = i;
    assert(msa->length % 3 == 0);
    for (l = 0; l < msa->nseqs; l++) msa->seqs[l][i] = '\0';
  }

  lst_free(block_begs);
  lst_free(block_ends);

  return retval;
}

/* Clean an alignment of indel artifacts.  Replace the chars
   adjacent to each indel and gapless subsequences of insufficient
   length with missing data characters.  Also eliminate any sites at
   which bases are present from too few species.  If a model is to be
   used that considers tuples of sites of size greater than 1, then an
   appropriate number of columns of missing data will be left between
   sites that were not adjacent in the original data set.  This
   routine ignores issues of frame (cf. msa_coding_clean, above). */
void msa_indel_clean(MSA *msa,  /* MSA to clean */
                     int indel_border, /* Number of chars adjacent to
                                          each indel to discard */
                     int min_nbases, /* Minimum number of consecutive
                                        gapless bases (per sequence) */
                     int min_nseqs, /* Minimum number of seqs */
                     int tuple_size, /* Size of tuples to be
                                        considered; tuple_size-1
                                        columns of missing data will
                                        be maintained between
                                        nonadjacent columns */
                     char mdata_char) { /* Missing data character to use */
  int i, j, k, first_base, nempty, nbases;
  int empty_col[msa->length];

  /* need explicit representation of alignment, at least for now */
  if (msa->seqs == NULL) 
    ss_to_msa(msa);

  /* first replace bases at indel boundaries and subseqs of
     insufficient length with missing data */
  for (j = 0; j < msa->nseqs; j++) {
    i = 0;
    first_base = -1;
    while (1) {
      /* find start of next indel */
      for (; i < msa->length && msa->seqs[j][i] != GAP_CHAR; i++);
      if (i == msa->length) break;

      if (first_base >= 0 && i - first_base < min_nbases)
        /* subsequence too short; replace remainder */
        for (k = first_base + indel_border; k < i; k++)
          msa->seqs[j][k] = mdata_char;
      
      else {
        /* subsequence okay; replace only bases preceding indel */
        for (k = 1; 
             k <= indel_border && i-k >= 0 && msa->seqs[j][i-k] != GAP_CHAR; 
             k++) 
          msa->seqs[j][i-k] = mdata_char;
      }

      /* find end of indel */
      for (; i < msa->length && msa->seqs[j][i] == GAP_CHAR; i++);
      if (i == msa->length) break;

      /* replace bases following indel */
      for (k = 0; 
           k < indel_border && i+k < msa->length && msa->seqs[j][i+k] != GAP_CHAR; 
           k++) 
        msa->seqs[j][i+k] = mdata_char;

      first_base = i;           /* first base in next subsequence */
    }
  }
  
  /* now replace columns having too few non-gap and non-missing bases */
  for (i = 0; i < msa->length; i++) {
    empty_col[i] = 0;
    for (j = 0, nbases = 0; j < msa->nseqs; j++) 
      if (msa->seqs[j][i] != GAP_CHAR && msa->seqs[j][i] != mdata_char)
        nbases++;
    if (nbases < min_nseqs) {
      for (j = 0; j < msa->nseqs; j++) 
        if (msa->seqs[j][i] != GAP_CHAR && msa->seqs[j][i] != mdata_char) 
          msa->seqs[j][i] = mdata_char;
      empty_col[i] = 1;
    }
  }

  /* now collapse all sequences of empty (completely missing) columns
     to length tuple_size-1; if tuple_size == 1, then all such columns
     will be removed */
  for (i = 0, k = 0, nempty = 0; i < msa->length; i++) {
    if (empty_col[i]) nempty++;
    else nempty = 0;

    if (nempty <= tuple_size-1 && !(empty_col[i] && k == 0)) {
      for (j = 0; j < msa->nseqs; j++) 
        msa->seqs[j][k] = (empty_col[i] ? mdata_char : msa->seqs[j][i]);
      k++;
    }
  }
  msa->length = k;
  if (nempty > 0) msa->length -= min(2, nempty);
  for (j = 0; j < msa->nseqs; j++) msa->seqs[j][msa->length] = '\0';
}


/* read specified filenames and concatenate to form one large
   alignment.  The list 'seqnames' will be used to define the order of
   the sequences in the combined MSA (missing sequences will be
   replaced with gaps).  All source MSAs must share the same alphabet,
   and each must contain a subset of the names in 'seqnames'.  */
MSA *msa_concat_from_files(List *fnames, msa_format_type format, 
                           List *seqnames, char *alphabet) {
  MSA *retval;
  int nseqs = lst_size(seqnames);
  Hashtable *name_hash = hsh_new(nseqs);
  MSA *source_msa = NULL;
  int i, j, k;
  FILE *F;
  char **tmpseqs = smalloc(nseqs * sizeof(char*));
  char **names = (char**)smalloc(nseqs * sizeof(char*));

  /* set up names for new MSA object */
  for (i = 0; i < nseqs; i++) {
    String *s = lst_get_ptr(seqnames, i);
    names[i] = (char*)smalloc(STR_SHORT_LEN * sizeof(char));
    strncpy(names[i], s->chars, STR_SHORT_LEN);
  }

  retval = msa_new(NULL, names, nseqs, 0, alphabet);

  /* build a hash with the index corresponding to each name */
  for (i = 0; i < nseqs; i++) 
    hsh_put(name_hash, names[i], (void*)i);

  for (i = 0; i < lst_size(fnames); i++) {
    String *fname = lst_get_ptr(fnames, i);
    if ((F = fopen(fname->chars, "r")) == NULL || 
        (source_msa = msa_new_from_file(F, format, alphabet)) == NULL) 
      die("ERROR: cannot read MSA from %s.\n", fname->chars);

    if (source_msa->seqs == NULL) {
      if (source_msa->ss == NULL || source_msa->ss->tuple_idx == NULL) 
        die("ERROR: msa_concat_from_files requires an ordered alignment.\n");
      ss_to_msa(source_msa);
    }

    assert(source_msa->seqs != NULL);

    /* reorder the seqs and names; add seqs of gaps as necessary */
    for (j = 0; j < nseqs; j++) tmpseqs[j] = NULL;
    for (j = 0; j < source_msa->nseqs; j++) {
      int idx = (int)hsh_get(name_hash, source_msa->names[j]);
      if (idx == -1) 
        die("ERROR: no match for sequence name '%s' in list.\n",
            source_msa->names[j]);
      tmpseqs[idx] = source_msa->seqs[j];
      source_msa->names[j] = srealloc(source_msa->names[j], 
                                      STR_SHORT_LEN * sizeof(char));
                                /* can't guarantee alloc any larger
                                   than current string */
    }
    if (source_msa->nseqs < nseqs) {
      source_msa->names = (char**)srealloc(source_msa->names, 
                                           nseqs * sizeof(char*));
      source_msa->seqs = (char**)srealloc(source_msa->seqs, 
                                          nseqs * sizeof(char*));
      for (j = source_msa->nseqs; j < nseqs; j++)
        source_msa->names[j] = (char*)smalloc(STR_SHORT_LEN * sizeof(char));
      source_msa->nseqs = nseqs;
    }
    for (j = 0; j < nseqs; j++) {
      if (tmpseqs[j] == NULL) {
        source_msa->seqs[j] = (char*)smalloc((source_msa->length+1) * 
                                             sizeof(char));
        for (k = 0; k < source_msa->length; k++) 
          source_msa->seqs[j][k] = GAP_CHAR;
        source_msa->seqs[j][source_msa->length] = '\0';
      }        
      else 
        source_msa->seqs[j] = tmpseqs[j];
      strncpy(source_msa->names[j], names[j], STR_SHORT_LEN);
    }

    /* now concatenate the source MSA to the aggregate */
    msa_concatenate(retval, source_msa);

    msa_free(source_msa);
    fclose(F);
  }

  hsh_free(name_hash);
  free(tmpseqs);
  return retval;
}

/* Concatenate one MSA onto another.  Both alignments must have the
   same number of sequences and their order must correspond.  The
   sequence names of 'source_msa' will be ignored. */
void msa_concatenate(MSA *aggregate_msa, MSA *source_msa) {
  int i, j;
  assert(aggregate_msa->nseqs == source_msa->nseqs);

  if (aggregate_msa->seqs == NULL) {
    aggregate_msa->seqs = (char**)smalloc(aggregate_msa->nseqs * sizeof(char*));
    aggregate_msa->alloc_len = 0;
  }

  if (aggregate_msa->alloc_len == 0) {
    aggregate_msa->alloc_len = aggregate_msa->length + source_msa->length;
    for (j = 0; j < aggregate_msa->nseqs; j++)
      aggregate_msa->seqs[j] = 
        (char*)smalloc((aggregate_msa->alloc_len+1) * sizeof(char));
  }

  else if (aggregate_msa->length + source_msa->length > 
           aggregate_msa->alloc_len) {
    aggregate_msa->alloc_len += source_msa->length * 2;
    for (j = 0; j < aggregate_msa->nseqs; j++) 
      aggregate_msa->seqs[j] = 
        (char*)srealloc(aggregate_msa->seqs[j], 
                       (aggregate_msa->alloc_len+1) * sizeof(char));
  }

  for (i = 0; i < source_msa->length; i++) 
    for (j = 0; j < aggregate_msa->nseqs; j++)
      aggregate_msa->seqs[j][i+aggregate_msa->length] = source_msa->seqs[j][i];

  aggregate_msa->length += source_msa->length;
  for (j = 0; j < aggregate_msa->nseqs; j++)
    aggregate_msa->seqs[j][aggregate_msa->length] = '\0';
}

/* Randomly permute the columns of a multiple alignment.  */
void msa_permute(MSA *msa) {
  int i, j;
  int *rand_perm = smalloc(msa->length * sizeof(int));
  char **tmpseq = smalloc(msa->nseqs * sizeof(char*));

  for (i = 0; i < msa->nseqs; i++) 
    tmpseq[i] = smalloc(msa->length * sizeof(char));

  /* for now require explicit representation of alignment */
  if (msa->seqs == NULL && msa->ss != NULL) {
    ss_to_msa(msa);
    ss_free(msa->ss);
    msa->ss = NULL;
  }

  for (i = 0; i < msa->nseqs; i++)
    for (j = 0; j < msa->length; j++) 
      tmpseq[i][j] = msa->seqs[i][j];

  permute(rand_perm, msa->length);
  for (i = 0; i < msa->nseqs; i++)
    for (j = 0; j < msa->length; j++) 
      msa->seqs[i][j] = tmpseq[i][rand_perm[j]];

  for (i = 0; i < msa->nseqs; i++) free(tmpseq[i]);
  free(tmpseq);
  free(rand_perm);
}


/* reorder rows of MSA so that names match specified target order.
   All names in the msa must be present in target_order.  Rows of
   missing data will be added for names that are in target_order but
   not in msa */
void msa_reorder_rows(MSA *msa, List *target_order) {

  int *new_to_old = smalloc(lst_size(target_order) * sizeof(int));
  int *covered = smalloc(msa->nseqs * sizeof(int));
  char **new_names, **new_seqs;
  int i, j;

  for (i = 0; i < msa->nseqs; i++) covered[i] = 0;
  for (i = 0; i < lst_size(target_order); i++) {
    new_to_old[i] = msa_get_seq_idx(msa, ((String*)lst_get_ptr(target_order, i))->chars);
    if (new_to_old[i] >= 0) {
      assert(covered[new_to_old[i]] == 0); /* prohibits mult. refs */
      covered[new_to_old[i]] = 1;
    }
  }
  for (i = 0; i < msa->nseqs; i++) {
    if (!covered[i]) {
      fprintf(stderr, "ERROR (msa_reorder_rows): name '%s' missing from reorder list.\n", msa->names[i]);
      exit(1);
    }
  }

  /* if both seqs and suff stats are present, discard the suff stats */
  if (msa->seqs != NULL && msa->ss != NULL) {
    ss_free(msa->ss);
    msa->ss = NULL;
  }

  /* reorder names */
  new_names = smalloc(lst_size(target_order) * sizeof(char*));
  for (i = 0; i < lst_size(target_order); i++) {
    if (new_to_old[i] >= 0) new_names[i] = msa->names[new_to_old[i]];
    else new_names[i] = strdup(((String*)lst_get_ptr(target_order, i))->chars);
  }
  free(msa->names);
  msa->names = new_names;

  if (msa->seqs != NULL) {      /* explicit seqs only */
    /* reorder seqs */
    new_seqs = smalloc(lst_size(target_order) * sizeof(char*));
    for (i = 0; i < lst_size(target_order); i++) {
      if (new_to_old[i] >= 0) new_seqs[i] = msa->seqs[new_to_old[i]];
      else {
        new_seqs[i] = smalloc((msa->length + 1) * sizeof(char));
        for (j = 0; j < msa->length; j++) new_seqs[i][j] = msa->missing[0];
        new_seqs[i][msa->length] = '\0';
      }
    }
    free(msa->seqs);
    msa->seqs = new_seqs;
  }
  else {                        /* suff stats only */
    assert(msa->ss != NULL);
    ss_reorder_rows(msa, new_to_old, lst_size(target_order));
  }

  /* finally, update nseqs */
  msa->nseqs = lst_size(target_order);

  free(new_to_old);
  free(covered);
}

/* return character for specified sequence and position; provides a
   layer of indirection to handle cases where sufficient stats are and
   are not used */
char msa_get_char(MSA *msa, int seq, int pos) {
  if (msa->seqs != NULL) return msa->seqs[seq][pos];
  else return ss_get_char_pos(msa, pos, seq, 0);
}

/* get format type indicated by string */
msa_format_type msa_str_to_format(char *str) {
  if (!strcmp(str, "MPM")) return MPM;
  else if (!strcmp(str, "FASTA")) return FASTA;
  else if (!strcmp(str, "SS")) return SS;
  else if (!strcmp(str, "LAV")) return LAV;
  else if (!strcmp(str, "PHYLIP")) return PHYLIP;
  else if (!strcmp(str, "MAF")) return MAF;
  else if (!strcmp(str, "LAV")) return LAV;
  return -1;
}

/** Return format type indicated by filename suffix */
msa_format_type msa_format_for_suffix(char *fname) {
  msa_format_type retval = -1;
  String *s = str_new_charstr(fname);
  str_suffix(s, '.');
  if (str_equals_charstr(s, "mpm")) retval = MPM;
  else if (str_equals_charstr(s, "fa")) retval = FASTA;
  else if (str_equals_charstr(s, "ss")) retval = SS;
  else if (str_equals_charstr(s, "lav")) retval = LAV;
  else if (str_equals_charstr(s, "ph")) retval = PHYLIP;
  else if (str_equals_charstr(s, "maf")) retval = MAF;
  else if (str_equals_charstr(s, "lav")) retval = LAV;
  str_free(s);
  return retval;
}

/** Return appropriate filename suffix for format type */
char *msa_suffix_for_format(msa_format_type t) {
  switch (t) {
  case FASTA:
    return "fa";
  case PHYLIP:
    return "ph";
  case MPM:
    return "mpm";
  case SS:
    return "ss";
  case MAF:
    return "maf";
  default:
    return "msa";
  }
}

/* remove N from alphabet; sometimes useful when fitting tree models */
void msa_remove_N_from_alph(MSA *msa) {
  int i, j;
  for (i = 0, j = 0; i < strlen(msa->alphabet); i++) 
    if (msa->alphabet[i] != 'N') {
      msa->alphabet[j++] = msa->alphabet[i];
    }
  msa->alphabet[j] = '\0';
  msa->inv_alphabet[(int)'N'] = -1;
}

/* for use with a reference-sequence alignment: find sites in the
   multiple alignment which consist only of the reference sequence,
   and appear to have no real alignment information.  These are taken
   to be sites that belong to blocks at least min_block_size in length
   with only the reference sequence and gaps or missing data in all
   other sequences.  The array 'missing' will be filled in with 1s
   (indicating no alignment information) or 0s.  It must be
   preallocated to size msa->length.  An ordered representation of the
   alignment is required.  NOTE: this routine now can typically be
   replaced by the simpler one below, because of better handling of
   missing data */
void msa_find_noaln(MSA *msa, int refseqidx, int min_block_size, int *noaln) {
  int j, k, run_start = -1, allbutref;
  assert(msa->seqs != NULL || (msa->ss != NULL && msa->ss->tuple_idx != NULL));
  for (j = 0; j < msa->length; j++) {
    noaln[j] = 0; 
    allbutref = msa_missing_col(msa, refseqidx, j);
    if (run_start == -1 && allbutref)
      run_start = j;
    else if (run_start >= 0 && !allbutref) {
      if (j - run_start >= min_block_size) 
        for (k = run_start; k < j; k++) noaln[k] = 1;
      run_start = -1;
    }
  }
  if (run_start >= 0)   /* no alignment at end */
    for (k = run_start; k < msa->length; k++) noaln[k] = 1;
}

/** Returns TRUE if alignment has missing data in all seqs but the
   reference seq at specified column; otherwise returns FALSE */
int msa_missing_col(MSA *msa, int ref, int pos) {
  int i;
  for (i = 0; i < msa->nseqs; i++) {
    if (i == ref-1) continue;
    if (!msa->is_missing[(int)msa_get_char(msa, i, pos)])
      return FALSE;
  }
  return TRUE;
}

/** Given a list of sequence names and/or 1-based indices, return a
    list of corresponding 0-based indices.  Warn if a name has no
    match.  Useful in converting command-line arguments */
List *msa_seq_indices(MSA *msa, List *seqnames) {
  int i, j;
  List *retval = lst_new_int(lst_size(seqnames));
  for (i = 0; i < lst_size(seqnames); i++) {
    String *name = lst_get_ptr(seqnames, i);
    int idx;
    if (str_as_int(name, &idx) == 0) {
      if (idx <= 0 || idx > msa->nseqs) 
        die("ERROR: sequence index %d is out of bounds.\n", idx);
      lst_push_int(retval, idx - 1);
    }
    else {
      for (j = 0; j < msa->nseqs; j++) {
        if (str_equals_charstr(name, msa->names[j])) {
          lst_push_int(retval, j);
          break;
        }
      }
      if (j == msa->nseqs) 
        fprintf(stderr, "WARNING: No match for name \"%s\" in alignment.\n", name->chars);
    }
  }
  return retval;
}

/** Mask out all alignment gaps of length greater than k by changing
    gap characters to missing data characters.  If refseq is > 0, the
    designated sequence will not be altered.  This function is useful
    when modeling micro-indels.  Warning: if MSA is stored only in
    terms of sufficient statistics, an explicit alignment will be
    created */
void msa_mask_macro_indels(MSA *msa, int k, int refseq) {
  int seq;
  
  if (msa->seqs == NULL && (msa->ss == NULL || msa->ss->tuple_idx == NULL))
    die("ERROR: ordered alignment required for msa_mask_macro_indels.\n");

  if (msa->seqs == NULL) ss_to_msa(msa);

  for (seq = 0; seq < msa->nseqs; seq++) {
    int i, j, len = 0, prev_is_gap = FALSE;
    if (seq == refseq - 1) continue;
    for (i = 0; i < msa->length; i++) {
      if (msa->seqs[seq][i] == GAP_CHAR) { /* gap open or extension */
        len++; 
        if (!prev_is_gap) prev_is_gap = TRUE;
      }
      else if (prev_is_gap) {   /* gap close */
        if (len > k)
          for (j = i - len; j < i; j++) 
            msa->seqs[seq][j] = msa->missing[0];
        len = 0;
        prev_is_gap = FALSE;
      }
    }
    if (prev_is_gap && len > k) /* long gap at end */ 
      for (j = i - len; j < i; j++) 
        msa->seqs[seq][j] = msa->missing[0];
  }

  if (msa->ss != NULL) {        /* have to rebuild; beware of stale
                                   pointers to old version */
    int tuple_size = msa->ss->tuple_size;
    ss_free(msa->ss);
    msa->ss = NULL;
    ss_from_msas(msa, tuple_size, TRUE, NULL, NULL, NULL, -1);
  }
}

/** Set up array indicating which sequences are to be considered
    "informative", e.g., for phylogenetic analysis */
void msa_set_informative(MSA *msa, /**< Alignment */
                         List *not_informative 
                                /** List of names of sequences *not*
                                    to be considered informative */
                         ) {
  int i;
  List *indices = msa_seq_indices(msa, not_informative);
  msa->is_informative = smalloc(msa->nseqs * sizeof(int));
  for (i = 0; i < msa->nseqs; i++) msa->is_informative[i] = TRUE;
  for (i = 0; i < lst_size(indices); i++)
    msa->is_informative[lst_get_int(indices, i)] = FALSE;
  lst_free(indices);
}

/* reset alphabet of MSA */
void msa_reset_alphabet(MSA *msa, char *newalph) {
  int i, nchars = strlen(newalph);
  free(msa->alphabet);  
  msa->alphabet = smalloc((nchars + 1) * sizeof(char));
  strcpy(msa->alphabet, newalph); 
  for (i = 0; i < nchars; i++) { 
    msa->inv_alphabet[i] = -1;
    msa->is_missing[i] = 0;
  }
  for (i = 0; msa->alphabet[i] != '\0'; i++)
    msa->inv_alphabet[(int)msa->alphabet[i]] = i;
  for (i = 0; msa->missing[i] != '\0'; i++)
    msa->is_missing[(int)msa->missing[i]] = 1;
}

/* convert all missing data characters to gaps, except for Ns in the
   reference sequence (if refseq > 0).  These will be replaced by
   randomly chosen bases (only use if Ns in reference sequence are rare) */
void msa_missing_to_gaps(MSA *msa, int refseq) {
  int i, j, k;

  assert(msa->seqs != NULL || msa->ss != NULL);

  if (msa->ss != NULL) {
    for (i = 0; i < msa->ss->ntuples; i++) {
      for (j = 0; j < msa->nseqs; j++) {
        for (k = 0; k < msa->ss->tuple_size; k++) {
          char c = ss_get_char_tuple(msa, i, j, -k);
          if (msa->is_missing[(int)c]) {
            if (j == refseq - 1 && c == 'N') {
              int char_idx = 4.0 * random()/RAND_MAX;
              set_col_char_in_string(msa, msa->ss->col_tuples[i], j, 
                                     msa->ss->tuple_size, -k, 
                                     msa->alphabet[char_idx]);
            }
            else
              set_col_char_in_string(msa, msa->ss->col_tuples[i], j, 
                                     msa->ss->tuple_size, -k, GAP_CHAR);
          }
        }
      }
    }
  }
  if (msa->seqs != NULL) {
    for (i = 0; i < msa->nseqs; i++) {
      for (j = 0; j < msa->length; j++) {
        if (msa->is_missing[(int)msa->seqs[i][j]]) {
          if (i == refseq - 1 && msa->seqs[i][j] == 'N') {
            int char_idx = 4.0 * random()/RAND_MAX;
            msa->seqs[i][j] = msa->alphabet[char_idx];
          }
          else
            msa->seqs[i][j] = GAP_CHAR;
        }
      }
    }
  }
}

/* return TRUE if alphabet has lowercase letters, FALSE otherwise */
int msa_alph_has_lowercase(MSA *msa) {
  int i;
  for (i = 0; msa->alphabet[i] != '\0'; i++)
    if (msa->alphabet[i] >= 'a' && msa->alphabet[i] <= 'z')
      return TRUE;
  return FALSE;
}

/* replace all lowercase characters with uppercase characters, adjust
   alphabet accordingly */
void msa_toupper(MSA *msa) {
  int i, j, k;

  assert(msa->seqs != NULL || msa->ss != NULL);

  for (i = 0, j = 0; msa->alphabet[i] != '\0'; i++) {
    if (msa->alphabet[i] >= 'a' && msa->alphabet[i] <= 'z') {
      char newc = toupper(msa->alphabet[i]);
      msa->inv_alphabet[(int)msa->alphabet[i]] = -1; /* remove lower case */
      if (msa->inv_alphabet[(int)newc] < 0) {
        /* replace with new upper case version */
        msa->alphabet[j] = newc; 
        msa->inv_alphabet[(int)newc] = j;
        j++;
      }
      /* otherwise don't add; upper case version already in alphabet */
    }
    else                        /* not lower case; keep in alphabet */
      msa->alphabet[j++] = msa->alphabet[i];
  }
  msa->alphabet[j] = '\0';    

  /* now replace all lowercase chars in alignment */
  if (msa->ss != NULL) {
    for (i = 0; i < msa->ss->ntuples; i++) 
      for (j = 0; j < msa->nseqs; j++) 
        for (k = 0; k < msa->ss->tuple_size; k++) 
          set_col_char_in_string(msa, msa->ss->col_tuples[i], j, 
                                 msa->ss->tuple_size, -k, 
                                 toupper(ss_get_char_tuple(msa, i, j, -k)));
  }
  if (msa->seqs != NULL) {                        
    for (i = 0; i < msa->nseqs; i++) 
      for (j = 0; j < msa->length; j++) 
        msa->seqs[i][j] = toupper(msa->seqs[i][j]);
  }
}
