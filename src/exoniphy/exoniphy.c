/* $Id: exoniphy.c,v 1.15 2004-06-30 06:39:40 acs Exp $
   Written by Adam Siepel, 2002-2004
   Copyright 2002-2004, Adam Siepel, University of California */


#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <getopt.h>
#include <string.h>
#include <phylo_hmm.h>
#include <gff.h>
#include <category_map.h>
#include <sufficient_stats.h>
#include <stringsplus.h>

/* default background feature types; used when scoring predictions and
   reflecting HMM */
#define DEFAULT_BACKGD_CATS "background,CNS"

/* default "cds" and "signal" feature tupes */
#define DEFAULT_CDS_CATS "CDS,start_codon,cds5'ss,cds3'ss"
#define DEFAULT_SIGNAL_CATS "stop_codon,5'splice,3'splice,prestart"
                                /* cat names that aren't present will
                                   be ignored */

/* parameters controlling evaluation of Sn/Sp tradeoff (see -Y option) */
#define SCALE_RANGE_MIN -20
#define SCALE_RANGE_MAX 10
#define NSENS_SPEC_TRIES 10

void print_usage() {
    printf("\n\
PROGRAM:    exoniphy\n\
\n\
USAGE:      exoniphy <msa_fname> > predictions.gff\n\
\n\
    Required argument <msa_fname> must be a multiple alignment\n\
    file, in one of several possible formats (see --msa-format).\n\
\n\
DESCRIPTION: \n\
\n\
    Prediction of evolutionarily conserved protein-coding exons using\n\
    a phylogenetic hidden Markov model (phylo-HMM).  By default, a\n\
    model definition and model parameters are used that are\n\
    appropriate for exon prediction in human DNA, based on\n\
    human/mouse/rat alignments and a 60-state HMM.  Using the --hmm,\n\
    --tree-models, and --catmap options, however, it is possible to\n\
    define alternative phylo-HMMs, e.g., for prediction of exon pairs\n\
    or complete gene structures.\n\
\n\
\n\
EXAMPLES:\n\
    (coming soon)    \n\
\n\
OPTIONS:\n\
\n\
 (Model definition and model parameters)\n\
    --hmm, -H <fname>\n\
        Name of HMM file defining states and transition probabilities.\n\
        By default, the 60-state HMM described in Siepel & Haussler\n\
        (2004) is used, with transition probabilities appropriate for\n\
        mammalian genomes (estimated as described in that paper).\n\
\n\
    --tree-models, -m <fname_list>\n\
        List of tree model (*.mod) files, one for each state in the\n\
        HMM.  Order of models must correspond to order of states in\n\
        HMM file.  By default, a set of models appropriate for human,\n\
        mouse, and rat are used, estimated as described in Siepel &\n\
        Haussler (2004).\n\
\n\
    --catmap, -c <fname>|<string>\n\
        Mapping of feature types to category numbers.  Can give either\n\
        a filename or an \"inline\" description of a simple category\n\
        map, e.g., --catmap \"NCATS = 3 ; CDS 1-3\".  By default, a\n\
        category map is used that is appropriate for the 60-state HMM\n\
        mentioned above (see --hmm).\n\
\n\
 (Input and output)\n\
    --msa-format, -i PHYLIP|FASTA|MPM|SS \n\
        (default SS) File format of input alignment.\n\
 \n\
    --seqname, -s <name>\n\
        Use specified string as the \"seqname\" field in GFF output\n\
        (e.g., chr22).  By default, the filename root of the input\n\
        file is used.\n\
\n\
    --grouptag, -g <tag>\n\
        Use specified string as the tag denoting groups in GFF output\n\
        (default is \"exon_id\").\n\
\n\
    --score, -S\n\
        Report log-odds scores for predictions, equal to their log\n\
        total probability under an exon model minus their log total\n\
        probability under a background model.  The exon model can be\n\
        altered using --cds-types and --signal-types and the\n\
        background model can be altered using --backgd-types (see below).\n\
\n\
 (Altering the states and transition probabilities of the HMM)\n\
    --no-cns, -x \n\
        Eliminate the state/category for conserved noncoding sequence\n\
        from the default HMM and category map.  Ignored if non-default\n\
        HMM and category map are selected.\n\
\n\
    --reflect-strand, -U \n\
        Given an HMM describing the forward strand, create a larger\n\
        HMM that allows for features on both strands by \"reflecting\"\n\
        the HMM about all states associated with background categories\n\
        (see --backgd-cats).  The new HMM will be used for predictions\n\
        on both strands.  If the default HMM is used, then this option\n\
        will be used automatically.\n\
\n\
    --bias, -b <val>\n\
        Set \"coding bias\" equal to the specified value (default 0).\n\
        The coding bias is added to the log probabilities of\n\
        transitions from background states to non-background states\n\
        (see --backgd-cats), then all transition probabilities are\n\
        renormalized.  If the coding bias is positive, then more\n\
        predictions will tend to be made and sensitivity will tend to\n\
        improve, at some cost to specificity; if it is negative, then\n\
        fewer predictions will tend to be made, and specificity will\n\
        tend to improve, at some cost to sensitivity.\n\
\n\
    --sens-spec, -Y <fname-root>\n\
        Make predictions for a range of different coding\n\
        biases (see --bias), and write results to files with given\n\
        filename root.  This allows the sensitivity/specificity\n\
        tradeoff to be examined.  The range is fixed at %d to %d, \n\
        and %d different sets of predictions are produced.\n\
\n\
 (Feature types)\n\
    --cds-types, -C <list>\n\
        Feature types that represent protein-coding regions (default\n\
        value: \"%s\").  Used when scoring\n\
        predictions and filling out 'frame' field in GFF output.\n\
\n\
    --backgd-types, -B <list>\n\
        Feature types to be considered \"background\" (default value:\n\
        \"%s\").  Affects --reflect-strand, --score, and --bias.\n\
\n\
    --signal-types, -L <list>\n\
        (for use with --score) Types of features to be considered\n\
        \"signals\" during scoring (default value: \n\
        \"%s\").  One score is produced \n\
        for each CDS feature (as defined by --cds-types) and \n\
        adjacent signal features; the score is then assigned to\n\
        the CDS feature.\n\
\n\
 (Indels and G+C content)\n\
    --indels, -I\n\
        Use the indel model described in Siepel & Haussler (2004).\n\
\n\
    --no-gaps, -W <list>\n\
        Prohibit gaps in sites of the specified categories (gaps result in\n\
        emission probabilities of zero).  If the default category map\n\
        is used (see --catmap), then gaps are prohibited in start and\n\
        stop codons and at the canonical GT and AG positions of splice\n\
        sites (with or without --indels).  In all other cases, the\n\
        default behavior is to treat gaps as missing data, or to address\n\
        them with the indel model implied by --indels.\n\
\n\
    --gc-ranges, -D <range-cutoffs>\n\
        (Changes interpretation of --models) Use different sets of\n\
        tree models, depending on the G+C content of the input\n\
        alignment.  The list <range-cutoffs> must consist of x ordered\n\
        values in (0,1), defining x+1 G+C classes.  The argument to\n\
        --models must then consist of the names of x+1 files, each of\n\
        which contains a list of tree-model filenames.\n\
\n\
 (Other)\n\
    --quiet, -q \n\
        Proceed quietly (without messages to stderr).\n\
\n\
    --help -h\n\
        Print this help message.\n\
\n\
\n\
REFERENCES:\n\
 \n\
    A. Siepel and D. Haussler.  2004.  Computational identification of\n\
      evolutionarily conserved exons.  Proc. 8th Annual Int'l Conf.\n\
      on Research in Computational Biology (RECOMB '04), pp. 177-186.\n\n", 
           SCALE_RANGE_MIN, SCALE_RANGE_MAX, NSENS_SPEC_TRIES, 
           DEFAULT_CDS_CATS, DEFAULT_BACKGD_CATS, DEFAULT_SIGNAL_CATS);
}

int main(int argc, char* argv[]) {

  /* variables for options, with defaults */
  int msa_format = SS;
  int quiet = FALSE, reflect_hmm = FALSE, score = FALSE, indels = FALSE, 
    no_cns = FALSE;
  double bias = NEGINFTY;
  char *seqname = NULL, *grouptag = "exon_id", *sens_spec_fname_root = NULL;
  List *model_fname_list = NULL, *no_gaps_str = NULL, *gc_thresholds = NULL;
  List *backgd_cats = get_arg_list(DEFAULT_BACKGD_CATS), 
    *cds_cats = get_arg_list(DEFAULT_CDS_CATS), 
    *signal_cats = get_arg_list(DEFAULT_SIGNAL_CATS);

  struct option long_opts[] = {
    {"hmm", 1, 0, 'H'},
    {"tree-models", 1, 0, 'm'},
    {"catmap", 1, 0, 'c'},
    {"msa-format", 1, 0, 'i'},
    {"seqname", 1, 0, 's'},
    {"grouptag", 1, 0, 'g'},
    {"score", 0, 0, 'S'},
    {"no-cns", 0, 0, 'x'},
    {"reflect-strand", 0, 0, 'U'},
    {"bias", 1, 0, 'b'},
    {"sens-spec", 1, 0, 'Y'},
    {"cds-types", 1, 0, 'C'},
    {"backgd-types", 1, 0, 'B'},
    {"signal-types", 1, 0, 'L'},
    {"indels", 0, 0, 'I'},
    {"no-gaps", 1, 0, 'W'},
    {"gc-ranges", 1, 0, 'D'},
    {"quiet", 0, 0, 'q'},
    {"help", 0, 0, 'h'},
  };

  /* other variables */
  FILE* F;
  PhyloHmm *phmm;
  MSA *msa;
  TreeModel **mod;
  HMM *hmm = NULL;
  CategoryMap *cm = NULL;
  GFF_Set *predictions;
  char c;
  int i, ncats, ncats_unspooled, trial, ntrials, opt_idx;
  double gc;
  char tmpstr[STR_LONG_LEN];
  String *fname_str = str_new(STR_LONG_LEN), *str;

  while ((c = getopt_long(argc, argv, "i:c:H:m:s:g:B:T:L:IW:b:D:xSYUhq", 
                          long_opts, &opt_idx)) != -1) {
    switch(c) {
    case 'i':
      msa_format = msa_str_to_format(optarg);
      if (msa_format == -1) die("ERROR: bad alignment format.\n");
      break;
    case 'c':
      cm = cm_new_string_or_file(optarg);
      break;
    case 'm':
      model_fname_list = get_arg_list(optarg);
      break;
    case 'H':
      hmm = hmm_new_from_file(fopen_fname(optarg, "r"));
      break;
    case 'x':
      no_cns = TRUE;
      break;
    case 'U':
      reflect_hmm = TRUE;
      break;
    case 'B':
      lst_free_strings(backgd_cats); lst_free(backgd_cats); /* free defaults */
      backgd_cats = get_arg_list(optarg);
      break;
    case 'T':
      lst_free_strings(cds_cats); lst_free(cds_cats); /* free defaults */
      cds_cats = get_arg_list(optarg);
      break;
    case 'L':
      lst_free_strings(signal_cats); lst_free(signal_cats); /* free defaults */
      signal_cats = get_arg_list(optarg);
      break;
    case 'S':
      score = TRUE;
      break;
    case 'b':
      bias = get_arg_dbl(optarg);
      break;
    case 'Y':
      sens_spec_fname_root = optarg;
      break;
    case 'W':
      no_gaps_str = get_arg_list(optarg);
      break;
    case 'I':
      indels = TRUE;
      break;
    case 'D':
      gc_thresholds = str_list_as_dbl(get_arg_list(optarg));
      for (i = 0; i < lst_size(gc_thresholds); i++)
        if (lst_get_dbl(gc_thresholds, i) <= 0 || 
            lst_get_dbl(gc_thresholds, i) >= 1 ||
            (i > 0 && lst_get_dbl(gc_thresholds, i-1) >=
             lst_get_dbl(gc_thresholds, i)))
          die("ERROR: Bad args to --gc-ranges.\n");
      break; 
    case 's':
      seqname = optarg;
      break;
    case 'g':
      grouptag = optarg;
      break;
    case 'q':
      quiet = TRUE;
      break;
    case 'h':
      print_usage();
      exit(0);
    case '?':
      fprintf(stderr, "ERROR: unrecognized option.  Try 'exoniphy -h' for help.\n");
      exit(1);
    }
  }

  if (optind != argc - 1) 
    die("ERROR: alignment filename is required argument.  Try 'exoniphy -h' for help.\n");

  if (gc_thresholds != NULL && 
      lst_size(model_fname_list) != lst_size(gc_thresholds) + 1)
    die("ERROR: with --gc-ranges, number of args to --tree-models must be exactly\none more than number of args to --gc-ranges.  Try 'exoniphy -h' for help.\n");

  if (sens_spec_fname_root != NULL && bias != NEGINFTY)
    die("ERROR: can't use --bias and --sens-spec together.\n");

  /* set hmm, tree models, and category map to defaults, if not
     already specified */
  if (hmm == NULL) {
    char *default_hmm = "default.hmm";
    if (indels) {
      if (no_cns) default_hmm = "default-indels-no-cns.hmm";
      else default_hmm = "default-indels.hmm";
    }
    else if (no_cns) default_hmm = "default-no-cns.hmm";
    sprintf(tmpstr, "%s/data/exoniphy/mammals/%s", PHAST_HOME, default_hmm);
    if (!quiet) fprintf(stderr, "Reading default HMM from %s...\n", tmpstr);
    hmm = hmm_new_from_file(fopen_fname(tmpstr, "r"));
    reflect_hmm = TRUE;
  }

  if (model_fname_list == NULL) {
    model_fname_list = lst_new_ptr(10);
    sprintf(tmpstr, "%s/data/exoniphy/%s", PHAST_HOME, no_cns ? "models-no-cns" : 
            "models");
    str_slurp(fname_str, fopen_fname(tmpstr, "r"));
    str_split(fname_str, NULL, model_fname_list);
    if (!quiet) 
      fprintf(stderr, "Reading default tree models from %s/data/exoniphy/mammals...\n", PHAST_HOME);
    for (i = 0; i < lst_size(model_fname_list); i++) {
      str = lst_get_ptr(model_fname_list, i);
      sprintf(tmpstr, "%s/data/exoniphy/mammals/%s", PHAST_HOME, str->chars);
      str_cpy_charstr(str, tmpstr);
    }
  }

  if (cm == NULL) {
    sprintf(tmpstr, "%s/data/exoniphy/%s", PHAST_HOME, 
            no_cns ? "default-no-cns.cm" : "default.cm");
    if (!quiet) fprintf(stderr, "Reading default category map from %s...\n", tmpstr);
    cm = cm_read(fopen_fname(tmpstr, "r"));
    if (no_gaps_str == NULL) 
      no_gaps_str = get_arg_list("10,11,20,21,cds5\'ss,cds3\'ss,start_codon,stop_codon");
  }

  /* read alignment */
  if (!quiet)
    fprintf(stderr, "Reading alignment from %s ...\n", 
            !strcmp(argv[optind], "-") ? "stdin" : argv[optind]);
  
  msa = msa_new_from_file(fopen_fname(argv[optind], "r"), msa_format, NULL);
  msa_remove_N_from_alph(msa);
  if (msa_format == SS && msa->ss->tuple_idx == NULL) 
    die("ERROR: Ordered representation of alignment required.\n");

  /* use filename root for default seqname */
  if (seqname == NULL) {
    String *tmp = str_new_charstr(argv[optind]);
    str_remove_path(tmp);
    str_root(tmp, '.');
    seqname = tmp->chars;
  }

  ncats = cm->ncats + 1;
  ncats_unspooled = cm->unspooler != NULL ? cm->unspooler->nstates_unspooled : 
    ncats;

  /* if --gc-ranges, figure out which set of tree models to use */
  if (gc_thresholds != NULL) {
    String *gc_models_fname = NULL;
    gsl_vector *f = msa_get_base_freqs(msa, -1, -1); 
    gc = gsl_vector_get(f, msa->inv_alphabet[(int)'G']) +
      gsl_vector_get(f, msa->inv_alphabet[(int)'C']);
    for (i = 0; gc_models_fname == NULL && i < lst_size(gc_thresholds); i++)
      if (gc < lst_get_dbl(gc_thresholds, i))
        gc_models_fname = lst_get_ptr(model_fname_list, i);
    if (gc_models_fname == NULL) 
      gc_models_fname = lst_get_ptr(model_fname_list, lst_size(gc_thresholds));     
    if (!quiet) 
      fprintf(stderr, "G+C content is %.1f%%; using models for partition %d (%s) ...\n", gc*100, i, gc_models_fname->chars);
    
    /* this trick makes it as if the correct set of models had been
       specified directly */
    sprintf(tmpstr, "*%s", gc_models_fname->chars);
    lst_free_strings(model_fname_list); lst_clear(model_fname_list);
    model_fname_list = get_arg_list(tmpstr);
    
    gsl_vector_free(f);
  }

  /* read tree models */
  if (lst_size(model_fname_list) != ncats) 
    die("ERROR: number of tree models must equal number of site categories.\n");
    
  mod = (TreeModel**)smalloc(sizeof(TreeModel*) * ncats);
  for (i = 0; i < ncats; i++) {
    String *fname = (String*)lst_get_ptr(model_fname_list, i);
    F = fopen_fname(fname->chars, "r");
    mod[i] = tm_new_from_file(F); 
    mod[i]->use_conditionals = 1;
    fclose(F);
  }

  /* disallow gaps, if necessary */
  if (no_gaps_str != NULL) {
    List *l = cm_get_category_list(cm, no_gaps_str, 0);
    for (i = 0; i < lst_size(l); i++) 
      mod[lst_get_int(l, i)]->allow_gaps = FALSE;
    lst_free(l);
  }      

  phmm = phmm_new(hmm, mod, cm, reflect_hmm ? backgd_cats : NULL, 
                  indels, msa->nseqs);

  /* add bias, if necessary */
  if (bias != NEGINFTY) phmm_add_bias(phmm, backgd_cats, bias);

  /* compute emissions */
  phmm_compute_emissions(phmm, msa, quiet);

  /* now produce predictions.  Need to do this in a loop because
     of sens-spec mode */
  if (sens_spec_fname_root != NULL) {    
    phmm_add_bias(phmm, backgd_cats, SCALE_RANGE_MIN);
    ntrials = NSENS_SPEC_TRIES;
  }
  else ntrials = 1;

  for (trial = 0; trial < ntrials; trial++) {
        
    if (ntrials > 1 && !quiet)
      fprintf(stderr, "(Sensitivity/specificity trial #%d)\n", trial+1);

    /* run Viterbi */
    if (!quiet)
      fprintf(stderr, "Executing Viterbi algorithm...\n");
    predictions = phmm_predict_viterbi(phmm, seqname, grouptag, cds_cats);

    /* score predictions */
    if (score) {
      if (!quiet) fprintf(stderr, "Scoring predictions...\n");            
      phmm_score_predictions(phmm, predictions, cds_cats, 
                             signal_cats, backgd_cats, TRUE);
    }

    /* convert to coord frame of reference sequence and adjust for
       idx_offset.  FIXME: make clear in help page assuming refidx 1 */
    msa_map_gff_coords(msa, predictions, 0, 1, msa->idx_offset, NULL);

    /* output predictions */
    if (sens_spec_fname_root != NULL) { 
      char tmpstr[STR_MED_LEN];
      sprintf(tmpstr, "%s.v%d.gff", sens_spec_fname_root, trial+1);
      gff_print_set(fopen_fname(tmpstr, "w+"), predictions);      
      if (trial < ntrials - 1)     /* also set up for next iteration */
        phmm_add_bias(phmm, backgd_cats, (SCALE_RANGE_MAX - SCALE_RANGE_MIN)/
                      (NSENS_SPEC_TRIES-1));
    }
    else                        /* just output to stdout */
      gff_print_set(stdout, predictions);

  } /* ntrials loop */

  if (!quiet)
    fprintf(stderr, "Done.\n");

  return 0;
}
