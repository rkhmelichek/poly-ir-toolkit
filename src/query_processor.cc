// Copyright (c) 2010, Roman Khmelichek
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//  3. Neither the name of Roman Khmelichek nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

//==============================================================================================================================================================
// Author(s): Roman Khmelichek
//
//==============================================================================================================================================================

#include "query_processor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

#include "cache_manager.h"
#include "config_file_properties.h"
#include "configuration.h"
#include "external_index.h"
#include "globals.h"
#include "logger.h"
#include "meta_file_properties.h"
#include "timer.h"
using namespace std;

/**************************************************************************************************************************************************************
 * QueryProcessor
 *
 **************************************************************************************************************************************************************/
QueryProcessor::QueryProcessor(const char* index_filename, const char* lexicon_filename, const char* doc_map_filename, const char* meta_info_filename,
                               const char* stop_words_list_filename, QueryAlgorithm query_algorithm, QueryMode query_mode, ResultFormat result_format) :
  query_algorithm_(query_algorithm),
  query_mode_(query_mode),
  result_format_(result_format),
  max_num_results_(Configuration::GetResultValue<long int>(Configuration::GetConfiguration().GetNumericalValue(config_properties::kMaxNumberResults))),
  silent_mode_(false),  // TODO: Get from configuration file.
  warm_up_mode_(false),  // TODO: Get from configuration file.
  use_positions_(Configuration::GetResultValue(Configuration::GetConfiguration().GetBooleanValue(config_properties::kUsePositions))),
  collection_average_doc_len_(0),
  collection_total_num_docs_(0),
  external_index_reader_(GetExternalIndexReader(query_algorithm_)),
  cache_policy_(((Configuration::GetConfiguration().GetValue(config_properties::kMemoryMappedIndex) == "true") ?
                  static_cast<CacheManager*> (new MemoryMappedCachePolicy(index_filename)) :
                  (Configuration::GetConfiguration().GetValue(config_properties::kMemoryResidentIndex) == "true") ?
                    static_cast<CacheManager*> (new FullContiguousCachePolicy(index_filename)) :
                    static_cast<CacheManager*> (new LruCachePolicy(index_filename)))),
  index_reader_(IndexReader::kRandomQuery, IndexReader::kSortedGapCoded, *cache_policy_, lexicon_filename, doc_map_filename, meta_info_filename, use_positions_,
                external_index_reader_),
  index_layered_(false),
  index_overlapping_layers_(false),
  index_num_layers_(1),
  total_querying_time_(0),
  total_num_queries_(0),
  num_early_terminated_queries_(0),
  num_single_term_queries_(0),

  not_enough_results_definitely_(0),
  not_enough_results_possibly_(0),
  num_queries_containing_single_layered_terms_(0),
  num_queries_kth_result_meeting_threshold_(0),
  num_queries_kth_result_not_meeting_threshold_(0),

  num_postings_scored_(0),
  num_postings_skipped_(0) {
  if (max_num_results_ <= 0) {
    Configuration::ErroneousValue(config_properties::kMaxNumberResults, Configuration::GetConfiguration().GetValue(config_properties::kMaxNumberResults));
  }

  if (stop_words_list_filename != NULL) {
    LoadStopWordsList(stop_words_list_filename);
  }
  LoadIndexProperties();
  PrintQueryingParameters();

  // If the index resides in main memory or is memory mapped, we'll create a block level index to speed up "random" accesses and skips;
  // this way we can skip right to the block we want.
  bool in_memory_index = IndexConfiguration::GetResultValue(Configuration::GetConfiguration().GetBooleanValue(config_properties::kMemoryResidentIndex), false);
  bool memory_mapped_index = IndexConfiguration::GetResultValue(Configuration::GetConfiguration().GetBooleanValue(config_properties::kMemoryMappedIndex), false);

  // TODO: Using an in-memory block index (for standard DAAT-AND) does not provide us any benefit. Most likely, the blocks should be smaller, or we should instead index the chunk last docIDs.
  //       Sequential block search performs better than binary block search in this case.
  if (memory_mapped_index || in_memory_index) {
    if (query_algorithm_ != kDaatOr && query_algorithm_ != kTaatOr) {
      cout << "Building in-memory block level index." << endl;
      BuildBlockLevelIndex();
    }
  }

  // A parameter for batch mode processing. Helps with running some experiments. We just define it here (but perhaps better to make it more configurable).
  // The percentage of total batch queries to use to generate statistics.
  // The rest of the queries will be used to warm up the cache.
  float percentage_test_queries = 0.01f;

  switch (query_mode_) {
    case kInteractive:
    case kInteractiveSingle:
      AcceptQuery();
      break;
    case kBatchAll:
      // We redefine the parameter for this particular batch mode,
      // which runs and times the execution of the whole query log,
      // with no warm up and with silent mode enabled,
      // and does not shuffle the queries prior to running them.
      percentage_test_queries = 1.0f;
    case kBatch:
      RunBatchQueries(cin, percentage_test_queries);
      break;
    default:
      assert(false);
      break;
  }

  // Output some querying statistics.
  double total_cached_bytes_read = index_reader_.total_cached_bytes_read();
  double total_disk_bytes_read = index_reader_.total_disk_bytes_read();
  double total_num_queries_issued = total_num_queries_;

  cout << "Number of queries executed: " << total_num_queries_ << endl;
  cout << "Number of single term queries: " << num_single_term_queries_ << endl;
  cout << "Total querying time: " << total_querying_time_ << " seconds\n";

  cout << "\n";
  cout << "Early Termination Statistics:\n";
  cout << "Number of early terminated queries: " << num_early_terminated_queries_ << endl;
  cout << "not_enough_results_definitely_: " << not_enough_results_definitely_ << endl;
  cout << "not_enough_results_possibly_: " << not_enough_results_possibly_ << endl;
  cout << "num_queries_containing_single_layered_terms_: " << num_queries_containing_single_layered_terms_ << endl;
  cout << "num_queries_kth_result_meeting_threshold_: " << num_queries_kth_result_meeting_threshold_ << endl;
  cout << "num_queries_kth_result_not_meeting_threshold_: " << num_queries_kth_result_not_meeting_threshold_ << endl;

  cout << "Average postings scored: " << (num_postings_scored_ / total_num_queries_issued) << endl;
  cout << "Average postings skipped: " << (num_postings_skipped_ / total_num_queries_issued) << endl;

  cout << "\n";
  cout << "Per Query Statistics:\n";
  cout << "  Average data read from cache: " << (total_cached_bytes_read / total_num_queries_issued / (1 << 20)) << " MiB\n";
  cout << "  Average data read from disk: " << (total_disk_bytes_read / total_num_queries_issued / (1 << 20)) << " MiB\n";

  cout << "  Average query running time (latency): " << (total_querying_time_ / total_num_queries_issued * (1000)) << " ms\n";
}

QueryProcessor::~QueryProcessor() {
  delete external_index_reader_;
  delete cache_policy_;
}

void QueryProcessor::LoadStopWordsList(const char* stop_words_list_filename) {
  assert(stop_words_list_filename != NULL);

  std::ifstream ifs(stop_words_list_filename);
  if (!ifs) {
    GetErrorLogger().Log("Could not load stop word list file '" + string(stop_words_list_filename) + "'", true);
  }

  std::string stop_word;
  while (ifs >> stop_word) {
    stop_words_.insert(stop_word);
  }
}

// We iterate through the lexicon and decode all the block headers for the current inverted list.
// We then make a block level index by storing the last docID of each block for our current inverted list.
// Each inverted list layer will have it's own block level index (pointed to by the lexicon).
void QueryProcessor::BuildBlockLevelIndex() {
  // We make one long array for keeping all the block level indices.
  int num_per_term_blocks = IndexConfiguration::GetResultValue(index_reader_.meta_info().GetNumericalValue(meta_properties::kTotalNumPerTermBlocks), true);

  uint32_t* block_level_index = new uint32_t[num_per_term_blocks];
  int block_level_index_pos = 0;

  MoveToFrontHashTable<LexiconData>* lexicon = index_reader_.lexicon().lexicon();
  for (MoveToFrontHashTable<LexiconData>::Iterator it = lexicon->begin(); it != lexicon->end(); ++it) {
    LexiconData* curr_term_entry = *it;
    if (curr_term_entry != NULL) {
      int num_layers = curr_term_entry->num_layers();
      for (int i = 0; i < num_layers; ++i) {
        ListData* list_data = index_reader_.OpenList(*curr_term_entry, i, true);

        int num_chunks_left = curr_term_entry->layer_num_chunks(i);

        assert(block_level_index_pos < num_per_term_blocks);
        curr_term_entry->set_last_doc_ids_layer_ptr(block_level_index + block_level_index_pos, i);

        while (num_chunks_left > 0) {
          BlockDecoder* block = list_data->curr_block_decoder();

          // We index only the last chunk in each block that's related to our current term.
          // So we always use the last chunk in a block, except the last block of this list, since that last chunk might belong to another list.
          int total_num_chunks = block->num_chunks();  // The total number of chunks in our current block.
          int chunk_num = block->starting_chunk() + num_chunks_left;
          int last_list_chunk_in_block = ((total_num_chunks > chunk_num) ? chunk_num : total_num_chunks);

          uint32_t last_block_doc_id = block->chunk_last_doc_id(last_list_chunk_in_block - 1);

          assert(block_level_index_pos < num_per_term_blocks);
          block_level_index[block_level_index_pos++] = last_block_doc_id;

          num_chunks_left -= block->num_actual_chunks();

          if (num_chunks_left > 0) {
            // We're moving on to process the next block. This block is of no use to us anymore.
            list_data->AdvanceBlock();
          }
        }

        index_reader_.CloseList(list_data);
      }
    }
  }

  // If everything is correct, these should be equal at the end.
  assert(num_per_term_blocks == block_level_index_pos);

  // Reset statistics about how much we read from disk/cache and how many lists we accessed.
  index_reader_.ResetStats();
}

void QueryProcessor::AcceptQuery() {
  while (true) {
    cout << "Search: ";
    string queryLine;
    getline(cin, queryLine);

    if (cin.eof())
      break;

    ExecuteQuery(queryLine, 0);

    if (query_mode_ != kInteractive)
      break;
  }
}

int QueryProcessor::ProcessQuery(LexiconData** query_term_data, int num_query_terms, Result* results, int* num_results) {
  const int kMaxNumResults = *num_results;
  ListData* list_data_pointers[num_query_terms];  // Using a variable length array here.

  bool single_term_query = false;
  if (num_query_terms == 1) {
    if (!warm_up_mode_)
      ++num_single_term_queries_;
    single_term_query = true;
  }

  for (int i = 0; i < num_query_terms; ++i) {
    // Here, we always open the last layer for a term. This way, we can support standard querying on layered indices, however, if loading the entire
    // index into main memory, we'll also be loading list layers we'll never be using.
    // TODO: This only applies to indices with overlapping layers; need to check that first.
    //       Also need to override that the index is not layered, so that this function will be called.
    list_data_pointers[i] = index_reader_.OpenList(*query_term_data[i], query_term_data[i]->num_layers() - 1, single_term_query);
  }

  int total_num_results;
  switch (query_algorithm_) {
    case kDaatAnd:
      // Query terms must be arranged in order from shortest list to longest list.
      sort(list_data_pointers, list_data_pointers + num_query_terms, ListCompare());
      total_num_results = IntersectLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
      break;
    case kDaatOr:
      total_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
      break;
    case kDaatAndTopPositions:
      // Query terms must be arranged in order from shortest list to longest list.
      sort(list_data_pointers, list_data_pointers + num_query_terms, ListCompare());
      total_num_results = IntersectListsTopPositions(list_data_pointers, num_query_terms, results, kMaxNumResults);
      break;
    default:
      total_num_results = 0;
      assert(false);
  }

  *num_results = min(total_num_results, kMaxNumResults);
  for (int i = 0; i < num_query_terms; ++i) {
    index_reader_.CloseList(list_data_pointers[i]);
  }
  return total_num_results;
}

// Used by the query processing methods that utilize list layers.
void QueryProcessor::OpenListLayers(LexiconData** query_term_data, int num_query_terms, int max_layers, ListData* list_data_pointers[][MAX_LIST_LAYERS],
                                    bool* single_term_query, int* single_layer_list_idx, int* total_num_layers) {
#ifdef IRTK_DEBUG
  // Build the query string.
  string query;
  for (int i = 0; i < num_query_terms; ++i) {
    query += string(query_term_data[i]->term(), query_term_data[i]->term_len()) + string(" ");
  }
  cout << "Processing layered query: " << query << endl;
#endif

  *single_term_query = false;
  if (num_query_terms == 1) {
    if (!warm_up_mode_)
      ++num_single_term_queries_;
    *single_term_query = true;
  }

  *single_layer_list_idx = -1;
  *total_num_layers = 0;
  // Open up all the lists for processing (each layer of one list is considered a separate list for our purposes here).
  for (int i = 0; i < num_query_terms; ++i) {
    // Find the first list that's single layered (we'll be using this info to speed things up).
    if (query_term_data[i]->num_layers() == 1 && *single_layer_list_idx == -1) {
      *single_layer_list_idx = i;
    }

    for (int j = 0; j < max_layers; ++j) {
      // We might not always have all the layers.
      if (j < query_term_data[i]->num_layers()) {
        ++(*total_num_layers);
        list_data_pointers[i][j] = index_reader_.OpenList(*query_term_data[i], j, *single_term_query, i);

        if (!silent_mode_)
          cout << "Score threshold for list '" << string(query_term_data[i]->term(), query_term_data[i]->term_len()) << "', layer #" << j << " is: "
              << query_term_data[i]->layer_score_threshold(j) << ", num_docs: " << query_term_data[i]->layer_num_docs(j) << "\n";
      } else {
        // For any remaining layers we don't have, we just open up the last layer.
        list_data_pointers[i][j] = index_reader_.OpenList(*query_term_data[i], query_term_data[i]->num_layers() - 1, *single_term_query, i);
      }
    }

    if (!silent_mode_)
      cout << endl;
  }
}

void QueryProcessor::CloseListLayers(int num_query_terms, int max_layers, ListData* list_data_pointers[][MAX_LIST_LAYERS]) {
  for (int i = 0; i < num_query_terms; ++i) {
    for (int j = 0; j < max_layers; ++j) {
      index_reader_.CloseList(list_data_pointers[i][j]);
    }
  }
}


/*
 * TODO: for OR semantics, don't need to load block level index and do binary search --- but it is useful when we switch to AND mode...
 * Query 'and armadillo'--- it's odd that we rarely get early termination --- most of the time, we just get speedup from AND processing mode!
 */

// Remember that this technique is not score safe, but it still should be rank safe.
// Implements approach described by Anh/Moffat with improvements by Strohman/Croft, but with standard BM25 scoring, instead of impacts.
// Problem with the BM25 scoring is that to maintain the threshold value, we must maintain a heap (or search through an array).
// This is more expensive than in an impact sorted index, where the score is the same for all documents in a segment.
// TODO: What if we start maintaining the threshold only after we start AND mode processing?
int QueryProcessor::ProcessLayeredTaatPrunedEarlyTerminatedQuery(LexiconData** query_term_data, int num_query_terms, Result* results, int* num_results) {
  const int kMaxLayers = MAX_LIST_LAYERS;  // Assume our lists can contain this many layers.
  const int kMaxNumResults = *num_results;

  ListData* list_data_pointers[num_query_terms][kMaxLayers];  // Using a variable length array here.
  bool single_term_query;
  int single_layer_list_idx;
  int total_num_layers;
  OpenListLayers(query_term_data, num_query_terms, kMaxLayers, list_data_pointers, &single_term_query, &single_layer_list_idx, &total_num_layers);

  int total_num_results = 0;

  // TODO: We can only support queries of a certain length (32 words). Can fix this by doing unoptimized processing for the shortest lists which do not
  //       fit within the 32 word limit.
  assert(num_query_terms <= static_cast<int>((sizeof(uint32_t) * 8)));

  ListData* max_score_sorted_list_data_pointers[total_num_layers];  // Using a variable length array here.
  uint32_t max_num_accumulators = 0;
//  int max_layer_docs = 0;
//  uint32_t max_num_docs = 0;
  int curr_layer = 0;
  for (int i = 0; i < num_query_terms; ++i) {
    for (int j = 0; j < query_term_data[i]->num_layers(); ++j) {
      max_score_sorted_list_data_pointers[curr_layer] = list_data_pointers[i][j];
      total_num_results += list_data_pointers[i][j]->num_docs();
      ++curr_layer;

      max_num_accumulators += list_data_pointers[i][j]->num_docs();
//      if (list_data_pointers[i][j]->num_docs() > max_layer_docs) {
//        max_layer_docs = list_data_pointers[i][j]->num_docs();
//        max_num_docs += list_data_pointers[i][j]->num_docs();
//      }
    }
  }
  assert(curr_layer == total_num_layers);

  max_num_accumulators = min(max_num_accumulators, collection_total_num_docs_);
//  max_num_docs = min(max_num_docs, collection_total_num_docs_);

//  cout << "MAX NUM DOCS: " << max_num_docs << endl;


  /////TODo: DEBUG: process a layer in pure OR mode to check for docID
//  ListData* my_test_list[1];
//  my_test_list[0] = list_data_pointers[2][5];
//  total_num_results = MergeLists(my_test_list, 1, results, kMaxNumResults);
//  exit(1);
  //////////////////////////


  // Sort all the layers by their max score.
  sort(max_score_sorted_list_data_pointers, max_score_sorted_list_data_pointers + total_num_layers, ListLayerMaxScoreCompare());

  enum ProcessingMode {kAnd, kOr};
  ProcessingMode curr_processing_mode = kOr;

  // TODO: unclear how many accumulators we should create. Creating the max number for a layer might still not be enough (if you plan to keep using the same array).
  // Maybe create an array the size of the union of the docs in the segments, bounded by the number of docs in the whole collection...
  // We always start with a sorted accumulator array, so we do binary search to find the accumulator we want (where we update the starting location we binary search from since
  // the docIDs we're searching for are monotonically increasing).
  // We can also use some percentage of the total documents from each list
  // If a list is longer than D documents, then take only n% of documents from that list.
  // Here, we'd assume that if a list is very long, we'll early terminate before processing all of it's low scoring documents.
  // In this case, it's possible that we'd need to resize the accumulator structure -- so take that into account.

  // Alternatively, while we're in OR mode, use either a hash table or an array sized to the num documents in the collection (and the docID is the index into array).
  // Then, when we switch to AND mode, compact and sort the array, and use that.
  // Whenever we remove accumulators, at the end, we can just compact it.

  // TODO: Instead of storing accumulators in docID sorted order, what if we store it in score sorted order.
  //       Since the list is in main memory --- how much of a difference does it make, if we employ skips or not...?

  int accumulators_size = max_num_accumulators;
//  int accumulators_size = 1048576;// TODO: try picking just some number...
  Accumulator* accumulators = new Accumulator[accumulators_size];
  // Since our BM25 partial scores can be negative for documents from lists of very common terms, we need to set the initial threshold to the lowest possible score.
  float threshold = -numeric_limits<float>::max();
  float total_remainder = 0;  // The upperbound for the score of any new document encountered.
  int num_accumulators = 0;

  // Keep track of the threshold. This is a min heap.
  float top_k_scores[kMaxNumResults];  // Using a variable length array here.

  float term_upperbounds[num_query_terms];  // Using a variable length array here.

  for(int i = 0; i < total_num_layers; ++i) {
    // Check accumulators if we can switch to AND mode.
//      threshold = top_k_scores[0];//TODO: How to best track the threshold? Also...might need to account for updates of accumulators
    //                                  (if an accumulator is already inserted, we don't want to double count it + it's updated score, could lead to problem...
    ////////////////////////////////////// but is that only when we're not up to k accumulators yet??? NO --- it happens when the updated accumulator score pushes
    //////////////////////////////////////// out a lower accumulator score --- and this artificially bumps up the threshold ---.
    ///////////////////////////////////////
    /*
     * Can store pointers to the top-k accumulators. Then just write a comparison function...for the scores...and use the docID to make sure we don't insert duplicate accumulators...
     *
     *
     * While you're processing the segment --- can always start to create a new top-k heap
     * Since you'll be going through all the accumulators anyway, might as well do this here! Then no need to deal with updates accumulator scores!
     */

    // We calculate the remainder function here over all terms, to find the upperbound score
    // of any newly discovered docID.
//    cout << "Processing layer # " << i << ", with upperbound " << max_score_sorted_list_data_pointers[i]->score_threshold() << ", for term #" << max_score_sorted_list_data_pointers[i]->term_num() << endl;
    total_remainder = 0;
    for (int j = 0; j < num_query_terms; ++j) {
      for (int k = i; k < total_num_layers; ++k) {
        if (max_score_sorted_list_data_pointers[k]->term_num() == j) {
          total_remainder += max_score_sorted_list_data_pointers[k]->score_threshold();
          break;
        }
      }
    }
//    cout << "Remainder function (total upperbound): " << total_remainder << endl;

    // Set processing mode to AND if the conditions are right.
    if (total_remainder < threshold) {
//      cout << "SWITCHING TO AND MODE!, total_remainder: " << total_remainder << ", threshold: " << threshold << endl;
      curr_processing_mode = kAnd;
    }

//    /////////////////////////////////TODO
//    if (i == 17) {
//      for (int j = 0; j < (num_accumulators); ++j) {
//        cout << "accumulators[" << j << "]: " << accumulators[j].doc_id << endl;
//      }
//    }
//    ////////////////////////////////////

    // Accumulators should always be in docID sorted order before we start processing a layer.
    for (int j = 0; j < (num_accumulators - 1); ++j) {
      assert(accumulators[j+1].doc_id >= accumulators[j].doc_id);
    }

    // Process postings based on the mode we're in.
    switch (curr_processing_mode) {
      case kOr:
//        cout << "OR MODE" << endl;
        threshold = ProcessListLayer(max_score_sorted_list_data_pointers[i], &accumulators, &accumulators_size, &num_accumulators, top_k_scores, kMaxNumResults);
        break;
      case kAnd:
//        cout << "AND MODE" << endl;
        threshold = ProcessListLayerAnd(max_score_sorted_list_data_pointers[i], accumulators, num_accumulators, top_k_scores, kMaxNumResults);
        break;
      default:
        assert(false);
    }

    // Prune accumulators.
    // Compare the threshold value to the remainder function of each accumulator.
    // Remove accumulators whose upperbound is lower than the threshold.
    for (int j = 0; j < num_query_terms; ++j) {
      term_upperbounds[j] = 0;
      // We start at 'i+1' since we just processed this layer, and all accumulator scores are updated from within the current layer.
      for (int k = i + 1; k < total_num_layers; ++k) {
        if (max_score_sorted_list_data_pointers[k]->term_num() == j) {
          term_upperbounds[j] = max_score_sorted_list_data_pointers[k]->score_threshold();
          break;
        }
      }
//      cout << "Now, the upperbound is: " << term_upperbounds[j] << ", for term # : " << j << endl;
    }


    // Here we calculate the upperbound for each accumulator, and remove those that can't possibly exceed the threshold.
    // We also compact the accumulator table here too, by moving accumulators together.
    bool early_termination_condition_one = true; // No documents with current scores below the threshold can make it above the threshold.
    int num_invalidated_accumulators = 0;
    for (int j = 0; j < num_accumulators; ++j) {
      Accumulator& acc = accumulators[j];
      float acc_upperbound = acc.curr_score;
      for (int k = 0; k < num_query_terms; ++k) {
        if (((acc.term_bitmap >> k) & 1) == 0) {
          acc_upperbound += term_upperbounds[k];
        }
      }

      // Checks for the first of the early termination conditions.
      if (early_termination_condition_one && acc.curr_score < threshold && acc_upperbound > threshold) {
        early_termination_condition_one = false;
      }

      if (acc_upperbound < threshold) {
        // Remove accumulator.
        ++num_invalidated_accumulators;

        //////////////////// TODO: Debug
//        if (acc.doc_id == 1201796) {
//          cout << "Partial -- invalidating accumulator: " << acc.doc_id << endl;
//        }
        /////////////////////////////////

      } else {
        // We move the accumulator left, to compact the array. Note that this does not affect any accumulators beyond this one.
        accumulators[j - num_invalidated_accumulators] = acc;
      }
    }
    num_accumulators -= num_invalidated_accumulators;

//    cout << "num_invalidated_accumulators: " << num_invalidated_accumulators << endl;
//    cout << "num_accumulators: " << num_accumulators << endl;

    /*
     * TODO: At this stage, maybe it's better to just do lookups for the remaining accumulators (and we can early terminate the lookups too!).
     *       For each accumulator, just skip ahead into the list(s) for which we don't have a score yet.
     *       This must be done only after we've entered AND mode processing.
     *       Sort accumulators by docID, and make lookups into lists...
     *       Maybe would be good to have an overlapping layer?
     *       Problem we have towards the end --- that prevents us from early termination (maybe it's not really a problem).
     *         The upperbounds on accumulators are all the same (because of some low scoring layer that's yet to be processed),
     *         and we can't guarantee rank safety because the current scores are close.
     */

    // Check the other early termination condition.
    bool early_termination_condition_two = true;  // All documents with potential scores above the threshold cannot change their final order.
    if (early_termination_condition_one) {
      // Sort accumulators in ascending order.

//      cout << "CHECKING 2ND EARLY TERMINATION CONDITION!" << endl;

      /*
       * TODO: While in AND mode, we may skip certain accumulators completely (because there is no doc in the intersection),
       *       in this case, the upperbound may be wrong (but this is only on the last layer anyway), so it's a moot point....
       *       so the fact that our term upperbound is shown to be 0, but the score for it is missing, doesn't matter
       */


      sort(accumulators, accumulators + num_accumulators, AccumulatorScoreAscendingCompare());

      ///////////////// TODO: DEBUG!
//      cout << "NUM ACCUMULATORS: " << num_accumulators << endl;
//      for (int l = 0; l < min(num_accumulators, 10); ++l) {
//        cout << "L: " << l << ", " << accumulators[l].curr_score  << ", upperbound remaining: " << << endl;
//      }
      ////////////////////////////////////

      for (int j = 0; j < num_accumulators - 1; ++j) {
        // TODO: It might be a good idea to store upperbounds in the previous step inside the accumulator instead of recalculating
        //       But that depends on whether we have a lot of accumulators left, when we do this second step (we probably have few accumulators).
        float acc_upperbound = 0;
        for (int k = 0; k < num_query_terms; ++k) {
          if (((accumulators[j].term_bitmap >> k) & 1) == 0) {
            acc_upperbound += term_upperbounds[k];
//            cout << "missing score from term: " << k << ", upperbound contributions: " << term_upperbounds[k] << endl; // TODO: upperbound contribution shouldn't be 0!!!!
          }
        }

//        if(j < min(num_accumulators, 10)) {
//          cout << "accum_num: " << j << ", docID: " << accumulators[j].doc_id << ", score: " << accumulators[j].curr_score << ", upperbound: " << acc_upperbound << endl;
//        }
//        ////////////////////////////////////

        if (accumulators[j].curr_score == accumulators[j+1].curr_score && acc_upperbound > 0) {
          early_termination_condition_two = false;
          break;
        }

        if(acc_upperbound > (accumulators[j+1].curr_score - accumulators[j].curr_score)) {
          early_termination_condition_two = false;
          break;
        }
      }
    }

    // We can early terminate.
    if(early_termination_condition_one && early_termination_condition_two) {
      if(i < (total_num_layers - 1)) {
//        cout << "Early terminating at layer " << (i+1) << " out of " << total_num_layers << " total layers." << endl;
      }
      break;
    }

    // We need to keep the accumulators in docID sorted order.
    // TODO: Since we only add new accumulators in non-sorted order,
    //       we can sort all the new ones, and merge with the already sorted older ones (but this would require another accumulator array most likely!).
    sort(accumulators, accumulators + num_accumulators);  // Uses the internal operator<() of the Accumulator class to sort.
  }

  // Sort accumulators by score and return the top-k.
  sort(accumulators, accumulators + num_accumulators, AccumulatorScoreDescendingCompare());
  for (int i = 0; i < min(kMaxNumResults, num_accumulators); ++i) {
    results[i].first = accumulators[i].curr_score;
    results[i].second = accumulators[i].doc_id;
  }

  delete[] accumulators;

  // Clean up.
  for (int i = 0; i < num_query_terms; ++i) {
    for (int j = 0; j < kMaxLayers; ++j) {
      index_reader_.CloseList(list_data_pointers[i][j]);
    }
  }

  *num_results = min(num_accumulators, kMaxNumResults);
  return total_num_results;
}

// TODO: If the accumulator is sized to contain all docs in the collection, we can just update accumulators
//       by finding them by their docID as the index.
float QueryProcessor::ProcessListLayer(ListData* list, Accumulator** accumulators_array, int* accumulators_array_size, int* num_accumulators, float* top_k_scores, int k) {
  assert(list != NULL);
  assert(accumulators_array != NULL && *accumulators_array != NULL);
  assert(accumulators_array_size != NULL && *accumulators_array_size > 0);
  assert(*num_accumulators <= *accumulators_array_size);

  Accumulator* accumulators = *accumulators_array;
  int accumulators_size = *accumulators_array_size;

  // BM25 parameters: see 'http://en.wikipedia.org/wiki/Okapi_BM25'.
  const float kBm25K1 =  2.0;  // k1
  const float kBm25B = 0.75;  // b

  // We can precompute a few of the BM25 values here.
  const float kBm25NumeratorMul = kBm25K1 + 1;
  const float kBm25DenominatorAdd = kBm25K1 * (1 - kBm25B);
  const float kBm25DenominatorDocLenMul = kBm25K1 * kBm25B / collection_average_doc_len_;

  // BM25 components.
  float partial_bm25_sum;  // The BM25 sum for the current document we're processing in the intersection.
  int doc_len;
  uint32_t f_d_t;

  // Compute the inverse document frequency component. It is not document dependent, so we can compute it just once for this list.
  int num_docs_t = list->num_docs_complete_list();
  float idf_t = log10(1 + (collection_total_num_docs_ - num_docs_t + 0.5) / (num_docs_t + 0.5));

  int num_sorted_accumulators = *num_accumulators;  // This marks the point at which our newly inserted, unsorted accumulators start.
  int curr_accumulator_idx = 0;  // We start the search for a docID at the start of the accumulator table.
  uint32_t curr_doc_id = 0;
  int num_top_k_scores = 0;
  float threshold = 0;

  while ((curr_doc_id = index_reader_.NextGEQ(list, curr_doc_id)) < numeric_limits<uint32_t>::max()) {
    // Search for an accumulator corresponding to the current docID or insert if not found.
    while (curr_accumulator_idx < num_sorted_accumulators && accumulators[curr_accumulator_idx].doc_id < curr_doc_id) {
      // TODO: Maintain the threshold score.
      // This is for all the old accumulators, whose scores we won't be updating, but still need to be accounted for.
      threshold = KthScore(accumulators[curr_accumulator_idx].curr_score, top_k_scores, num_top_k_scores++, k);

      ++curr_accumulator_idx;
    }

    // Compute partial BM25 sum.
    f_d_t = index_reader_.GetFreq(list, curr_doc_id);
    doc_len = index_reader_.GetDocLen(curr_doc_id);
    partial_bm25_sum = idf_t * (f_d_t * kBm25NumeratorMul) / (f_d_t + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len);


//    //////////////////////TODO: DEBUG:
//    if(curr_doc_id == 1201796) {
//      cout << "Partial score for list with idf: " << idf_t << ", is: " << (partial_bm25_sum) << ", num_docs_t: " << num_docs_t << endl;
//    }
//    ////////////////////


    if (curr_accumulator_idx < num_sorted_accumulators && accumulators[curr_accumulator_idx].doc_id == curr_doc_id) { // Found a matching accumulator.
      accumulators[curr_accumulator_idx].curr_score += partial_bm25_sum;
      accumulators[curr_accumulator_idx].term_bitmap |= (1 << list->term_num());

      // TODO: Maintain the threshold score.
      // This is for the updated accumulator scores.
      threshold = KthScore(accumulators[curr_accumulator_idx].curr_score, top_k_scores, num_top_k_scores++, k);

      ++curr_accumulator_idx;
    } else { // Need to insert accumulator.
      if (*num_accumulators >= accumulators_size) {
        cout << "RESIZING ACCUMULATOR ARRAY: max_size: " << accumulators_size << ", curr_num_size: " << *num_accumulators << endl;
        // Resize accumulator array.
        *accumulators_array_size *= 2;
        Accumulator* new_accumulators = new Accumulator[*accumulators_array_size];
        // TODO: is memcpy faster here?
        for (int i = 0; i < *num_accumulators; ++i) {
          new_accumulators[i] = accumulators[i];
        }
        delete[] *accumulators_array;
        *accumulators_array = new_accumulators;

        accumulators = *accumulators_array;
        accumulators_size = *accumulators_array_size;

//        cout << "MADE A COPY!!!" << endl;
      }
      accumulators[*num_accumulators].doc_id = curr_doc_id;
      accumulators[*num_accumulators].curr_score = partial_bm25_sum;
      accumulators[*num_accumulators].term_bitmap = (1 << list->term_num());

      // TODO: Maintain the threshold score.
      // This is for the new accumulator scores.
      threshold = KthScore(accumulators[*num_accumulators].curr_score, top_k_scores, num_top_k_scores++, k);

      ++(*num_accumulators);
    }

    ++curr_doc_id;
  }


  // Sort the accumulator array by docID.
  // Note that we only really need to sort any new accumulators we inserted and merge it with the already sorted part of the array.
  // TODO: an in-place merge would still require a buffer if you want to take O(n) time instead of O(n log n)...
  //       this is probably what strohman/croft meant when saying they always needed to allocate a new array for each segment...

  // TODO: test your comparison operator.
  sort(accumulators + num_sorted_accumulators, accumulators + *num_accumulators);
  inplace_merge(accumulators, accumulators + num_sorted_accumulators, accumulators + *num_accumulators);

  // We return the threshold score.
  return threshold;
}

float QueryProcessor::ProcessListLayerAnd(ListData* list, Accumulator* accumulators, int num_accumulators, float* top_k_scores, int k) {
  // BM25 parameters: see 'http://en.wikipedia.org/wiki/Okapi_BM25'.
  const float kBm25K1 =  2.0;  // k1
  const float kBm25B = 0.75;  // b

  // We can precompute a few of the BM25 values here.
  const float kBm25NumeratorMul = kBm25K1 + 1;
  const float kBm25DenominatorAdd = kBm25K1 * (1 - kBm25B);
  const float kBm25DenominatorDocLenMul = kBm25K1 * kBm25B / collection_average_doc_len_;

  // BM25 components.
  float partial_bm25_sum;  // The BM25 sum for the current document we're processing in the intersection.
  int doc_len;
  uint32_t f_d_t;

  // Compute the inverse document frequency component. It is not document dependent, so we can compute it just once for this list.
  int num_docs_t = list->num_docs_complete_list();
  float idf_t = log10(1 + (collection_total_num_docs_ - num_docs_t + 0.5) / (num_docs_t + 0.5));

  int accumulator_offset = 0;
  uint32_t curr_doc_id;
  int num_top_k_scores = 0;
  float threshold = 0;
  while (accumulator_offset < num_accumulators) {

//    ///////////////TODO: DEBUG
//    if(accumulators[accumulator_offset].doc_id == 1201796) {
//      cout << "FOUND IT!!!!!!!" << endl;
//    }
//    ///////////////////////

    ////// TODO: NextGEQOld works correctly, the new one has a bug, doesn't find docID 1201796...
    curr_doc_id = index_reader_.NextGEQ(list, accumulators[accumulator_offset].doc_id);
    if (curr_doc_id == accumulators[accumulator_offset].doc_id) {
      // Compute partial BM25 sum.
      f_d_t = index_reader_.GetFreq(list, curr_doc_id);
      doc_len = index_reader_.GetDocLen(curr_doc_id);
      partial_bm25_sum = idf_t * (f_d_t * kBm25NumeratorMul) / (f_d_t + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len);

//      //////////////////////TODO: DEBUG:
//      if(curr_doc_id == 1201796) {
//        cout << "Partial score for list with idf (and mode): " << idf_t << ", is: " << (partial_bm25_sum) << ", num_docs_t: " << num_docs_t << endl;
//      }
//      ////////////////////

      // Update accumulator with the document score.
      accumulators[accumulator_offset].curr_score += partial_bm25_sum;
      accumulators[accumulator_offset].term_bitmap |= (1 << list->term_num());

      // TODO: Maintain the threshold score.
      // This is for the updated accumulator scores.
      threshold = KthScore(accumulators[accumulator_offset].curr_score, top_k_scores, num_top_k_scores++, k);
    } else {
      // TODO: Maintain the threshold score.
      // This is for all the old accumulators, whose scores we won't be updating, but still need to be accounted for.
      threshold = KthScore(accumulators[accumulator_offset].curr_score, top_k_scores, num_top_k_scores++, k);


//      //////////////////////TODO: DEBUG:
//      if(curr_doc_id == 1201796) {
//        cout << "(skipping it!!) Partial score for list with idf (and mode): " << idf_t << ", is: " << ", num_docs_t: " << num_docs_t << endl;
//      }
//      ////////////////////
    }

    ++accumulator_offset;
  }

  return threshold;
}

// This is used to keep track of the threshold value (the score of the k-th highest scoring accumulator).
// The 'max_scores' array is assumed to be the size of at least 'kth_score'.
// Returns the lowest score in the scores array if 'num_scores' is equal to 'kth_score' and 0 otherwise.
float QueryProcessor::KthScore(float new_score, float* scores, int num_scores, int kth_score) {
  // We use a min heap to determine the k-th largest score (the lowest score of the k scores we keep).
  // Notice that we don't have to explicitly make the heap, since it's assumed to be maintained from the start.
  if (num_scores < kth_score) {
    // We insert a document if we don't have k documents yet.
    scores[num_scores++] = new_score;
    push_heap(scores, scores + num_scores, greater<float> ());
  } else {
    if (new_score > scores[0]) {
      // We insert a score only if it is greater than the minimum score in the heap.
      pop_heap(scores, scores + kth_score, greater<float> ());
      scores[kth_score - 1] = new_score;
      push_heap(scores, scores + kth_score, greater<float> ());
    }
  }

  if (num_scores < kth_score) {
    return -numeric_limits<float>::max();
  }
  return scores[0];
}

// This is for overlapping layers.
// We can actually process more than 2 terms at a time as follows:
// say we have 3 lists, A, B, C (each with at most 2 layers, the higher levels having duplicated docID info):
// Process (A_1 x B x C), (B_1 x A x C), (C_1 x A x B), where A, B, C are the whole lists and A_1, B_1, C_1 are the first (or only) layers.
// Note that intersecting all 3 terms should give good skipping performance.
// We also assume, the whole index is in main memory.
// Now, we can also run all 3 intersections in parallel (this should be good given that all 3 lists are in main memory).
// Merge all the results using a heap. Or store the results in an array (which is then sorted) and the top results determined (preferable).
// We only need k results from each of the 3 separate intersections.

// The drawback here (for queries that have lists with all layers) is you have to scan all the 2nd layers twice for the number of lists you have in the query.
// (But we intersect with small lists and with good skipping performance (the index is in main memory, plus a block level index to skip decoding block headers),
// so it makes the costs acceptable).

// Intersecting for more than 2 layers is redundant right now. That's why for 3 or more term queries, we get pretty high latencies.
// IDEA: After each intersection, can already check the threshold...
// if there are 3 lists A,B,C, then A_1 x B_2 x C_2 determines the intersection of the top A documents with everything else...including the top B and C since the layers are overlapping.
// This is even true for 2 lists. After doing A_1 x B_2, we can check the kth score (if we got k scores in the intersection) against the threshold m(A_2) + m(B_1).
//
//

// TODO: Two Bugs you need to fix:
// TODO: FIX THIS ISSUE NEXT!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
//       * 10 results from each intersection is not enough since some of them are duplicates
//       * Some docIDs appear more than once in the final results -- because heapifying only by score (the same docIDs have different scores for different intersections because of rounding errors during float addition).
//         SOLUTION: don't sort by score in IntersectLists(), sort by the docIDs for each intersection, merge docIDs (output array needs to be num_query_terms*num_results large to fit everything, in case all docIDs are unique).
//                   then after merging and eliminating duplicate docIDs, sort by score.
//     Solution to the first issue: Merge the results.
//
// TODO: Find percentage of queries that can early terminate in each category of number of query terms.
// TODO: Find out how much work is A_2 x B_2 vs A_1 x B_2 && B_1 x A_2. Do we traverse significantly less elements (when we are able to early terminate)? (And also for queries with more than 2 terms).
//       If the answer is yes, we traverse less elements --- then it would be good to keep the A_2, B_2 lists decompressed in main memory, with the BM25 score precomputed. Otherwise, the costs of decompression and BM25 computation are too large overheads.

/*
 * IDEA: Traverse all (or maybe some?) intersections in an interleaved manner. Then check threshold every N documents processed (after we get k unique docs) if we can early terminate on one of the intersections.
 *       We can also use the threshold info to skip chunks/blocks (if we store threshold info within the index or load it into main memory). This is because -- not all intersections are equal...some are gonna have lower max scores,
 *       the latter intersections due to IDF.
 *
 *
 */

/*
 * When running layered vs non-layered comparison tests --- some docIDs with the same score differ
 *
 *  TODO: Also some results just plain are different --- investigate this!!!
 *        Query: 'cam glacier national park'
 *
 *  TODO: Implement:
 *        Non overlapping index; keep an unresolved docID pool and an upperbound score so we can eliminate documents.
 *
 *  TODO: Might be better to time the whole batch of queries instead of individually timing each one and summing up the times.
 */
int QueryProcessor::ProcessLayeredQuery(LexiconData** query_term_data, int num_query_terms, Result* results, int* num_results) {
  const int kMaxLayers = MAX_LIST_LAYERS;  // Assume our lists can contain this many layers.
  const int kMaxNumResults = *num_results;

  ListData* list_data_pointers[num_query_terms][kMaxLayers];  // Using a variable length array here.
  bool single_term_query;
  int single_layer_list_idx;
  int total_num_layers;
  OpenListLayers(query_term_data, num_query_terms, kMaxLayers, list_data_pointers, &single_term_query, &single_layer_list_idx, &total_num_layers);

  // Run the appropriate intersections.
  ListData* curr_intersection_list_data_pointers[num_query_terms];  // Using a variable length array here.
  int total_num_results = 0;
  bool run_standard_intersection = false;
  if (single_layer_list_idx == -1) { // We have 2 layers for each term in the query.
    // For only 2 query terms, the other method is better.
    if (query_algorithm_ == kDualLayeredOverlappingMergeDaat && num_query_terms > 2) {
      // Here, we merge all the first layers together (while removing duplicate docIDs) and then treat it as one virtual list and intersect with all the 2nd layers.
      // We do the merge in an interleaved manner with the intersections to improve processing speed.
      // This allows us to avoid allocating an in-memory list (previous attempt to do this resulted in significantly slower running times).
      // This method will wind up traversing and scoring more documents, but it also a sort of way to do "bulk lookups".
      // I think this method, combined with docID reordering could provide even larger gains.

      ListData* merge_list_data_pointers[num_query_terms];  // Using a variable length array here.

      // Now do the intersection using the virtual list to drive which documents we're looking up.
      // Note that the virtual list could be larger than one of the 2nd layers.
      for (int i = 0; i < num_query_terms; ++i) {
        // Use only the first layer for each term.
        merge_list_data_pointers[i] = list_data_pointers[i][0];
        curr_intersection_list_data_pointers[i] = list_data_pointers[i][1];
      }

      sort(curr_intersection_list_data_pointers, curr_intersection_list_data_pointers + num_query_terms, ListCompare());
      total_num_results = IntersectLists(merge_list_data_pointers, num_query_terms, curr_intersection_list_data_pointers, num_query_terms, results, kMaxNumResults);
      *num_results = min(total_num_results, kMaxNumResults);
    } else {
      // TODO: I don't think it's enough for each intersection to return just k results
      //       when there might be duplicate docIDs that we'll be filtering...
      // This should probably be solved by writing a new function for intersect lists -- that will also combine the same docIDs from different list intersections right away
      // so we do a merge of all the results in one step...

      Result all_results[num_query_terms][kMaxNumResults]; // Using a variable length array here.
      int num_intersection_results[num_query_terms]; // Using a variable length array here.
      for (int i = 0; i < num_query_terms; ++i) {
        // Build the intersection list.
        // We always intersect with the first layer of each list.
        curr_intersection_list_data_pointers[i] = list_data_pointers[i][0];

        // We also intersect with all the second layers of all the other lists.
        for (int j = 0; j < num_query_terms; ++j) {
          if (j != i) {
            curr_intersection_list_data_pointers[j] = list_data_pointers[j][1];
          }
        }

        // List intersections must be arranged in order from shortest list to longest list.
        sort(curr_intersection_list_data_pointers, curr_intersection_list_data_pointers + num_query_terms, ListCompare());
        int curr_total_num_results = IntersectLists(curr_intersection_list_data_pointers, num_query_terms, all_results[i], kMaxNumResults);
        num_intersection_results[i] = min(curr_total_num_results, kMaxNumResults);
        total_num_results += curr_total_num_results;

        for (int j = 0; j < num_query_terms; ++j) {
          // Need to reset the 2nd layers after running the query since we'll be using them again in the next iteration.
          // In our current setup of 2 layers, we really need to only reset each 2nd layer once, and the 2nd time, it doesn't particularly matter.
          // But this is pretty cheap.
          if (curr_intersection_list_data_pointers[j]->layer_num() == 1) {
            curr_intersection_list_data_pointers[j]->ResetList(single_term_query);
          }
        }

        // Print results of individual intersections for debugging.
        if (!silent_mode_) {
          for (int j = 0; j < num_intersection_results[i]; ++j) {
            cout << all_results[i][j].second << ", score: " << all_results[i][j].first << endl;
          }
          cout << endl;
        }
      }

      // Merge the results from all the previous intersection(s) using a heap.

      // The 'pair<int, int>' is for keeping track of the index of the intersection as well as the index of the current Result entry within the intersection.
      pair<Result, pair<int, int> > result_heap[num_query_terms]; // Using a variable length array here.
      int result_heap_size = 0;
      for (int i = 0; i < num_query_terms; ++i) {
        if (num_intersection_results[i] > 0) {
          result_heap[i] = make_pair(all_results[i][0], make_pair(i, 1));
          ++result_heap_size;
          --num_intersection_results[i];
        }
      }

      make_heap(result_heap, result_heap + result_heap_size); // Default is max heap, which is what we want.
      int curr_result = 0;
      while (result_heap_size && curr_result < kMaxNumResults) {
        pop_heap(result_heap, result_heap + result_heap_size);

        Result& curr_top_result = result_heap[result_heap_size - 1].first;
        pair<int, int>& curr_top_result_idx = result_heap[result_heap_size - 1].second;

        // If the previous result we stored is the same as the current, we don't need to insert it.
        // We only compare the docIDs because the scores could be different when the order of the addition of the partial BM25 sums is different.
        // This is due to floating point rounding errors.
        if (curr_result == 0 || results[curr_result - 1].second != curr_top_result.second) {
          results[curr_result++] = curr_top_result;
        }

        int top_intersection_index = curr_top_result_idx.first;
        if (num_intersection_results[top_intersection_index] > 0) {
          --num_intersection_results[top_intersection_index];
          result_heap[result_heap_size - 1] = make_pair(all_results[top_intersection_index][curr_top_result_idx.second], make_pair(top_intersection_index,
                                                                                                                                   curr_top_result_idx.second
                                                                                                                                       + 1));
          push_heap(result_heap, result_heap + result_heap_size);
        } else {
          --result_heap_size;
        }
      }

      *num_results = curr_result;
    }

    // Need to satisfy the early termination conditions.

    // Check if we have enough results first.
    if (*num_results >= kMaxNumResults) {
      // We have enough results to possibly early terminate.
      // Check whether we meet the early termination requirements.
      Result& min_result = results[min(kMaxNumResults - 1, *num_results - 1)];
      float remaining_document_score_upperbound = 0;
      for (int i = 0; i < num_query_terms; ++i) {
        float bm25_partial_score = query_term_data[i]->layer_score_threshold(query_term_data[i]->num_layers() - 1);
        assert(!isnan(bm25_partial_score));
        remaining_document_score_upperbound += bm25_partial_score;
      }

      if (min_result.first > remaining_document_score_upperbound) {
        ////////////TODO: print the properly early terminated query.
        //        if(num_query_terms == 1) {
        //          static int QUERY_COUNT = 0;
        //          cout << QUERY_COUNT++ << ":" << query << endl;
        //        }
        ////////////////

        ++num_queries_kth_result_meeting_threshold_;
        if (!silent_mode_)
          cout << "Early termination possible!" << endl;

        if (!warm_up_mode_)
          ++num_early_terminated_queries_;
      } else {
        ++num_queries_kth_result_not_meeting_threshold_;
        if (!silent_mode_)
          cout << "Cannot early terminate due to score thresholds." << endl;

        run_standard_intersection = true;
      }

    } else {
      // Don't have enough results from the first layers, execute query on the 2nd layer.
      if (!warm_up_mode_ && *num_results < kMaxNumResults) {
        if (total_num_results < kMaxNumResults) {
          ++not_enough_results_definitely_;
          if (!silent_mode_)
            cout << "Definitely don't have enough results." << endl;
        } else {
          ++not_enough_results_possibly_;
          if (!silent_mode_)
            cout << "Potentially don't have enough results." << endl;
        }
      }

      run_standard_intersection = true;
    }
  } else {
    // If we have at least one term in the query that has only a single layer,
    // we can get away with doing only on intersection on the last layers of each inverted list.
    ++num_queries_containing_single_layered_terms_;
    if (!silent_mode_)
      cout << "Query includes term with only a single layer." << endl;

    run_standard_intersection = true;

    // We count this as an early terminated query.
    if (!warm_up_mode_)
      ++num_early_terminated_queries_;
  }

  if (run_standard_intersection) {
    // Need to re-run the query on the last layers for each list (this is actually the standard DAAT approach).
    for (int i = 0; i < num_query_terms; ++i) {
      // Before we rerun the query, we need to reset the list information so we start from the beginning.
      list_data_pointers[i][query_term_data[i]->num_layers() - 1]->ResetList(single_term_query);
      curr_intersection_list_data_pointers[i] = list_data_pointers[i][query_term_data[i]->num_layers() - 1];
    }

    sort(curr_intersection_list_data_pointers, curr_intersection_list_data_pointers + num_query_terms, ListCompare());
    total_num_results = IntersectLists(curr_intersection_list_data_pointers, num_query_terms, results, kMaxNumResults);
    *num_results = min(total_num_results, kMaxNumResults);
  }

  CloseListLayers(num_query_terms, kMaxLayers, list_data_pointers);

  // TODO: This is incorrect for some queries where we don't actually open and traverse the lower layers (such as one word queries).
  return total_num_results;
}


// Merges the lists into an in-memory list that only contains docIDs; it also removes duplicate docIDs that might be present in multiple lists.
// We do not score any documents here.
// TODO: We can also potentially score documents here and keep track of which lists the score came from, then we'd have to do less work scoring
//       when we intersect with the 2nd layers --- but the logic here would be more complicated.
//       Potentially we can also set up some thresholds...since we're doing OR mode processing --- look at the Efficient Query Processing in Main Memory paper...
int QueryProcessor::MergeLists(ListData** lists, int num_lists, uint32_t* merged_doc_ids, int max_merged_doc_ids) {
  pair<uint32_t, int> heap[num_lists];  // Using a variable length array here.
  int heap_size = 0;

  // Initialize the heap.
  for (int i = 0; i < num_lists; ++i) {
    uint32_t curr_doc_id;
    if ((curr_doc_id = index_reader_.NextGEQ(lists[i], 0)) < numeric_limits<uint32_t>::max()) {
      heap[heap_size++] = make_pair(curr_doc_id, i);
    }
  }

  // We use the default comparison --- which is fine, but the comparison for a pair checks both values, and we really only need to check the docID part
  // so it could be more efficient to write your own simple comparator.
  make_heap(heap, heap + heap_size, greater<pair<uint32_t, int> >());

  int i = 0;
  while (heap_size) {
    pair<uint32_t, int> top = heap[0];

    // Don't insert duplicate docIDs.
    assert(i < max_merged_doc_ids);
    if (i == 0 || merged_doc_ids[i - 1] != top.first) {
      merged_doc_ids[i++] = top.first;
    }

    // Need to pop and push to make sure heap property is maintained.
    pop_heap(heap, heap + heap_size, greater<pair<uint32_t, int> >());

    uint32_t curr_doc_id;
    if ((curr_doc_id = index_reader_.NextGEQ(lists[top.second], top.first + 1)) < numeric_limits<uint32_t>::max()) {
      heap[heap_size - 1] = make_pair(curr_doc_id, top.second);
      // TODO: OR Instead of making a new pair, can just update the pair, with the correct docID, and (possibly the list idx?, might depend on whether the heap size decreased previously).
      //       or maybe use the 'top' we have created.
      push_heap(heap, heap + heap_size, greater<pair<uint32_t, int> >());
    } else {
      --heap_size;
    }
  }

  return i;
}

// Standard DAAT OR mode processing for comparison purposes.
int QueryProcessor::MergeLists(ListData** lists, int num_lists, Result* results, int num_results) {
  // Setting this option to 'true' makes a considerable difference in average query latency (> 100ms).
  // When we score the complete doc, we first find the lowest docID in the array, and then scan the array for that docID, and completely score it.
  // All lists from which the docID was scored have their list pointers moved forward.
  // When we don't score the complete doc, at each turn of the while loop, we find a partial score of the lowest docID posting.
  // We add these together for a particular docID to get the complete score -- but it requires several iterations of the main while loop.
  // This is less efficient, since we have to do a complete linear search through the array for every posting.
  // On the other hand, when we score the complete doc right away, we only have to do one more linear search through all postings to score all the lists.
  // Clearly, if the majority of the docIDs are present in more than one list, we'll be getting a speedup.
  const bool kScoreCompleteDoc = true;

  // Use an array instead of a heap for selecting the list with the lowest docID at each step.
  // Using a heap for picking the list with the lowest docID is only implemented for when 'kScoreCompleteDoc' is false.
  // For compatibility with 'kScoreCompleteDoc' equal to true, you'd need to use the heap to choose the next list to score, instead of iterating through the array, which is what's done now.
  // Array based method is faster than the heap based method for choosing the lowest docID from all the lists, so this option should be set to 'true'.
  // TODO: Try another array based strategy: keep a sorted array of docIDs. When updating, only need to find the spot for the new docID and re-sort the array up to that spot.
  // TODO: Can also use a linked list for this. Then can just find the right spot, and do pointer changes. The locality here wouldn't be too good though.
  const bool kUseArrayInsteadOfHeapList = true;

  int total_num_results = 0;

  // BM25 parameters: see 'http://en.wikipedia.org/wiki/Okapi_BM25'.
  const float kBm25K1 =  2.0;  // k1
  const float kBm25B = 0.75;   // b

  // We can precompute a few of the BM25 values here.
  const float kBm25NumeratorMul = kBm25K1 + 1;
  const float kBm25DenominatorAdd = kBm25K1 * (1 - kBm25B);
  const float kBm25DenominatorDocLenMul = kBm25K1 * kBm25B / collection_average_doc_len_;

  // BM25 components.
  float bm25_sum = 0;  // The BM25 sum for the current document we're processing in the intersection.
  float partial_bm25_sum;
  int doc_len;
  uint32_t f_d_t;

  // Compute the inverse document frequency component. It is not document dependent, so we can compute it just once for each list.
  float idf_t[num_lists];  // Using a variable length array here.
  int num_docs_t;
  for (int i = 0; i < num_lists; ++i) {
    num_docs_t = lists[i]->num_docs_complete_list();
    idf_t[i] = log10(1 + (collection_total_num_docs_ - num_docs_t + 0.5) / (num_docs_t + 0.5));
  }

  // We use this to get the next lowest docID from all the lists.
  pair<uint32_t, int> lists_curr_postings[num_lists]; // Using a variable length array here.
  int num_lists_remaining = 0; // The number of lists with postings remaining.
  for (int i = 0; i < num_lists; ++i) {
    uint32_t curr_doc_id;
    if ((curr_doc_id = index_reader_.NextGEQ(lists[i], 0)) < numeric_limits<uint32_t>::max()) {
      lists_curr_postings[num_lists_remaining++] = make_pair(curr_doc_id, i);
    }
  }

  if (num_lists_remaining > 0) {
    if (!kUseArrayInsteadOfHeapList) {
      // We use our own comparator, that only checks the docID part.
      make_heap(lists_curr_postings, lists_curr_postings + num_lists_remaining, ListMaxDocIdCompare());
    }
  } else {
    return total_num_results;
  }

  // For the heap based method, the lowest element will always be the first element in the array.
  // So we can keep 'top' constant since it's just a pointer to the first element and just push/pop the heap.
  // For the array based method, we need to initialize it to the first element in the array, and then find the lowest value at the top of the while loop.
  // We have to find the lowest element here as well, since we need to initialize 'curr_doc_id' to the right value before we start the loop.
  pair<uint32_t, int>* top = &lists_curr_postings[0];
  if (kUseArrayInsteadOfHeapList) {
    for (int i = 1; i < num_lists_remaining; ++i) {
      if (lists_curr_postings[i].first < top->first) {
        top = &lists_curr_postings[i];
      }
    }
  }

  int i;
  uint32_t curr_doc_id = top->first;  // Current docID we're processing the score for.

  while (num_lists_remaining) {
    if (kUseArrayInsteadOfHeapList) {
      top = &lists_curr_postings[0];
      for (i = 1; i < num_lists_remaining; ++i) {
        if (lists_curr_postings[i].first < top->first) {
          top = &lists_curr_postings[i];
        }
      }
    }

    if(kScoreCompleteDoc) {
      curr_doc_id = top->first;
      bm25_sum = 0;
      // Can start searching from the position of 'top' since it'll be the first lowest element in the array.
      while (top != &lists_curr_postings[num_lists_remaining]) {
        if(top->first == curr_doc_id) {
          // Compute BM25 score from frequencies.
          f_d_t = index_reader_.GetFreq(lists[top->second], top->first);
          doc_len = index_reader_.GetDocLen(top->first);
          bm25_sum += idf_t[top->second] * (f_d_t * kBm25NumeratorMul) / (f_d_t + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len);

          ++num_postings_scored_;

          if ((top->first = index_reader_.NextGEQ(lists[top->second], top->first + 1)) == numeric_limits<uint32_t>::max()) {
            // Need to compact the array by one.
            // Just copy over the last value in the array and overwrite the top value, since we'll be removing it.
            // Now, we can declare our list one shorter.
            // If top happens to already point to the last value in the array, this step is superfluous.
          --num_lists_remaining;
            *top = lists_curr_postings[num_lists_remaining];
            --top;
          }
        }
        ++top;
      }

      // Need to keep track of the top-k documents.
      if (total_num_results < num_results) {
        // We insert a document if we don't have k documents yet.
        results[total_num_results] = make_pair(bm25_sum, curr_doc_id);
        push_heap(results, results + total_num_results + 1, ResultCompare());
      } else {
        if (bm25_sum > results->first) {
          // We insert a document only if it's score is greater than the minimum scoring document in the heap.
          pop_heap(results, results + num_results, ResultCompare());
          results[num_results - 1].first = bm25_sum;
          results[num_results - 1].second = curr_doc_id;
          push_heap(results, results + num_results, ResultCompare());
        }
      }
      ++total_num_results;
    } else {
      // Compute BM25 score from frequencies.
      f_d_t = index_reader_.GetFreq(lists[top->second], top->first);
      doc_len = index_reader_.GetDocLen(top->first);
      partial_bm25_sum = idf_t[top->second] * (f_d_t * kBm25NumeratorMul) / (f_d_t + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len);

      ++num_postings_scored_;

      // When we encounter the same docID as the current we'be been processing, we update it's score.
      // Otherwise, we know we're processing a new docID.
      if (top->first == curr_doc_id) {
        bm25_sum += partial_bm25_sum;
      } else if (top->first > curr_doc_id) {
        // Need to keep track of the top-k documents.
        if (total_num_results < num_results) {
          // We insert a document if we don't have k documents yet.
          results[total_num_results] = make_pair(bm25_sum, curr_doc_id);
          push_heap(results, results + total_num_results + 1, ResultCompare());
        } else {
          if (bm25_sum > results->first) {
            // We insert a document only if it's score is greater than the minimum scoring document in the heap.
            pop_heap(results, results + num_results, ResultCompare());
            results[num_results - 1].first = bm25_sum;
            results[num_results - 1].second = curr_doc_id;
            push_heap(results, results + num_results, ResultCompare());
          }
        }

        curr_doc_id = top->first;
        bm25_sum = partial_bm25_sum;
        ++total_num_results;
      } else {
        assert(false);
      }

      uint32_t next_doc_id;
      if ((next_doc_id = index_reader_.NextGEQ(lists[top->second], top->first + 1)) < numeric_limits<uint32_t>::max()) {
        if (kUseArrayInsteadOfHeapList) {
          top->first = next_doc_id;
        } else {
          // Need to pop and push to make sure heap property is maintained.
          pop_heap(lists_curr_postings, lists_curr_postings + num_lists_remaining, ListMaxDocIdCompare());
          lists_curr_postings[num_lists_remaining - 1].first = next_doc_id;
          push_heap(lists_curr_postings, lists_curr_postings + num_lists_remaining, ListMaxDocIdCompare());
        }
      } else {
        if (kUseArrayInsteadOfHeapList) {
          // Need to compact the array by one.
          // Just copy over the last value in the array and overwrite the top value, since we'll be removing it.
          // Now, we can declare our list one shorter.
          // If top happens to already point to the last value in the array, this step is superfluous.
          *top = lists_curr_postings[num_lists_remaining - 1];
        } else {
          pop_heap(lists_curr_postings, lists_curr_postings + num_lists_remaining, ListMaxDocIdCompare());
        }

        --num_lists_remaining;
      }
    }
  }

  if (!kScoreCompleteDoc) {
    // We always have a leftover result that we need to insert.
    // Note that there is no need to push the heap since we'll just be sorting all the results by their score next.
    if (total_num_results < num_results) {
      // We insert a document if we don't have k documents yet.
      results[total_num_results] = make_pair(bm25_sum, curr_doc_id);
    } else {
      if (bm25_sum > results->first) {
        // We insert a document only if it's score is greater than the minimum scoring document in the heap.
        pop_heap(results, results + num_results, ResultCompare());
        results[num_results - 1].first = bm25_sum;
        results[num_results - 1].second = curr_doc_id;
      }
    }
    ++total_num_results;
  }

  // Sort top-k results in descending order by document score.
  sort(results, results + min(num_results, total_num_results), ResultCompare());

  return total_num_results;
}

// The two tiered WAND first merges (OR mode) and scores the top docs lists, so that we know the k-th threshold (a better approximation of the lower bound).
// TODO: It seems to me that a two tiered WAND doesn't save us any computation. We'll be evaluating the top docs lists and then we'll be able to skip some docIDs from the 2nd layers.
//       NOTE: It WOULD save computation if the number of crappy, low scoring docIDs we'll be able to skip (due to an initially high threshold)
//             exceeds the extra number of top docs docIDs we had to compute scores for.
//       It would be really beneficial if you could decrease the upperbounds on the term lists for the 2nd layers
//       (which you can't unless you don't discard the top docs and store them in accumulators).
// In standard WAND, the k-th threshold is initialized to 0.
int QueryProcessor::MergeListsWand(LexiconData** query_term_data, int num_query_terms, Result* results, int* num_results, bool two_tiered) {
  // Constraints on the type of index we expect.
  assert(index_layered_);
  assert(index_overlapping_layers_);
  assert(index_num_layers_ == 2);

  const int kMaxNumResults = *num_results;

  // Holds a pointer to the list for each corresponding query term.
  ListData* list_data_pointers[num_query_terms];  // Using a variable length array here.

  // For WAND to work correctly, need term upperbounds on the whole list.
  float list_thresholds[num_query_terms];  // Using a variable length array here.

  bool single_term_query = false;
  if (num_query_terms == 1) {
    single_term_query = true;
  }

  for (int i = 0; i < num_query_terms; ++i) {
    // Open the first layer (the top docs).
    list_data_pointers[i] = index_reader_.OpenList(*query_term_data[i], 0, single_term_query);
    list_thresholds[i] = list_data_pointers[i]->score_threshold();
#ifdef IRTK_DEBUG
    cout << "Top Docs Layer for '" << string(query_term_data[i]->term(), query_term_data[i]->term_len())
        << "', Layer Num: 0, Score Threshold: " << list_data_pointers[i]->score_threshold()
        << ", Num Docs: " << list_data_pointers[i]->num_docs() << endl;
#endif
  }

  int total_num_results = 0;
  if (num_query_terms == 1) {
    // Do standard DAAT OR mode processing, since WAND won't help.
    if (index_layered_ && query_term_data[0]->num_layers() == 2) {
      // We have two layers, so let's run the standard DAAT OR on the first layer only.
      // If there are k results, we can stop; otherwise rerun the query on the second layer.
      total_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
      if (total_num_results < kMaxNumResults) {
        index_reader_.CloseList(list_data_pointers[0]);
        list_data_pointers[0] = index_reader_.OpenList(*query_term_data[0], query_term_data[0]->num_layers() - 1, single_term_query);
        total_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
      }
    } else {
      // There is only one layer, run the query on it.
      total_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
    }
  } else {
    /*
     * We can estimate the threshold after processing the top docs lists in OR mode, but we can't decrease the upperbounds on the 2nd layers
     * because this will result in many of our high scoring documents to be skipped from the 2nd layers (including the ones from the top docs lists).
     *
     * TODO: What are some ways of decreasing the upperbound on the 2nd layers...?
     */

    float threshold = 0;
    if (two_tiered) {
      // It's possible that after processing the top docs, there is an unresolved docID (only present in some of the top docs lists, but not in others)
      // that could have a score higher than the top-k threshold we derive here.
      // For this reason, we can't early terminate here if we get k results.
      int top_docs_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
#ifdef IRTK_DEBUG
      cout << "Num results from top docs lists: " << top_docs_num_results << endl;
#endif

      // The k-th score in the heap we get from the union of the top docs layers is our starting threshold.
      // It is a lower bound for the score necessary for a new docID to make it into our top-k.
      // The threshold is 0 if we didn't get k results from the top docs layers, meaning any docID can make it into the top-k.
      threshold = (top_docs_num_results >= kMaxNumResults) ? results[kMaxNumResults - 1].first : 0;
#ifdef IRTK_DEBUG
      cout << "Threshold from top docs lists: " << threshold << endl;
#endif
    }

    // We have to make sure that the layers are overlapping. So we'll be traversing the top-docs twice (in the second overlapping layer).
    // This is necessary because we're not using accumulators for the top-docs lists. It's only an approximate lower bound score on the top docIDs, since
    // the docID may be present in other lists, that did not make it into the top-docs.
    for (int i = 0; i < num_query_terms; ++i) {
      if (query_term_data[i]->num_layers() == 1) {
        // For a single layered list, we'll have to traverse it again.
        list_data_pointers[i]->ResetList(single_term_query);
      } else {
        // For a dual layered list, we close the first layer and open the second layer.
        index_reader_.CloseList(list_data_pointers[i]);
        list_data_pointers[i] = index_reader_.OpenList(*query_term_data[i], query_term_data[i]->num_layers() - 1, single_term_query);
      }

#ifdef IRTK_DEBUG
      cout << "Overlapping Layer for '" << string(query_term_data[i]->term(), query_term_data[i]->term_len())
          << "', Layer Num: " << (query_term_data[i]->num_layers() - 1)
          << ", Score Threshold: " << list_data_pointers[i]->score_threshold()
          << ", Num Docs: " << list_data_pointers[i]->num_docs() << endl;
#endif
    }

    const bool kMWand = true;

    // BM25 parameters: see 'http://en.wikipedia.org/wiki/Okapi_BM25'.
    const float kBm25K1 =  2.0;  // k1
    const float kBm25B = 0.75;  // b

    // We can precompute a few of the BM25 values here.
    const float kBm25NumeratorMul = kBm25K1 + 1;
    const float kBm25DenominatorAdd = kBm25K1 * (1 - kBm25B);
    const float kBm25DenominatorDocLenMul = kBm25K1 * kBm25B / collection_average_doc_len_;

    // BM25 components.
    float bm25_sum;  // The BM25 sum for the current document we're processing in the intersection.
    int doc_len;
    uint32_t f_d_t;

    // Compute the inverse document frequency component. It is not document dependent, so we can compute it just once for each list.
    float idf_t[num_query_terms];  // Using a variable length array here.
    int num_docs_t;
    for (int i = 0; i < num_query_terms; ++i) {
      num_docs_t = list_data_pointers[i]->num_docs_complete_list();
      idf_t[i] = log10(1 + (collection_total_num_docs_ - num_docs_t + 0.5) / (num_docs_t + 0.5));
    }

    // We use this to get the next lowest docID from all the lists.
    pair<uint32_t, int> lists_curr_postings[num_query_terms]; // Using a variable length array here.
    int num_lists_remaining = 0; // The number of lists with postings remaining.
    uint32_t curr_doc_id;
    for (int i = 0; i < num_query_terms; ++i) {
      if ((curr_doc_id = index_reader_.NextGEQ(list_data_pointers[i], 0)) < numeric_limits<uint32_t>::max()) {
        lists_curr_postings[num_lists_remaining++] = make_pair(curr_doc_id, i);
      }
    }

    int i, j;
    pair<uint32_t, int> pivot = make_pair(0, -1);  // The pivot can't be a pointer to the 'lists_curr_postings'
                                                   // since those values will change when we advance list pointers after scoring a docID.
    float pivot_weight;                            // The upperbound score on the pivot docID.

    /*
     * Two implementation choices here:
     * * Keep track of the number of lists remaining; requires an if statement after each nextGEQ() to check if we reached the max docID sentinel value (implemented here).
     * * Don't keep track of the number of lists remaining. Don't need if statement after each nextGEQ(), but need to sort all list postings at every turn.
     */
    while (num_lists_remaining) {
      // Sort current postings in non-descending order.
      // Can also sort all entries less than or equal to the pivot docID and merge with all higher docIDs.
      // Although probably won't be faster unless we have a significant number of terms in the query.
      sort(lists_curr_postings, lists_curr_postings + num_lists_remaining, ListDocIdCompare());

      // Select a pivot.
      pivot_weight = 0;
      pivot.second = -1;
      for (i = 0; i < num_lists_remaining; ++i) {
        pivot_weight += list_thresholds[lists_curr_postings[i].second];
        if (pivot_weight >= threshold) {
          pivot = lists_curr_postings[i];
          break;
        }
      }

      /*
      // If using this, change the while condition to true. Don't need to check for sentinel value after nextGEQ(),
      // but need to sort all the list postings at each step.
      if(pivot.first == numeric_limits<uint32_t>::max()) {
        break;
      }
      */

      // If we don't have a pivot (the pivot list is -1), or if the pivot docID is the sentinel value for no more docs,
      // it means that no newly encountered docID can make it into the top-k and we can quit.
      if (pivot.second == -1) {
        break;
      }

      if (pivot.first == lists_curr_postings[0].first) {
        // We have enough weight on the pivot, so score all docIDs equal to the pivot (these can be beyond the pivot as well).
        // We know we have enough weight when the docID at the pivot list equals the docID at the first list.
        bm25_sum = 0;
        for(i = 0; i < num_lists_remaining && pivot.first == lists_curr_postings[i].first; ++i) {
          // Compute the BM25 score from frequencies.
          f_d_t = index_reader_.GetFreq(list_data_pointers[lists_curr_postings[i].second], lists_curr_postings[i].first);
          doc_len = index_reader_.GetDocLen(lists_curr_postings[i].first);
          bm25_sum += idf_t[lists_curr_postings[i].second] * (f_d_t * kBm25NumeratorMul) / (f_d_t + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len);

          ++num_postings_scored_;

          // Advance list pointer.
          if ((lists_curr_postings[i].first = index_reader_.NextGEQ(list_data_pointers[lists_curr_postings[i].second], lists_curr_postings[i].first + 1)) == numeric_limits<uint32_t>::max()) {
            // Compact the array. Move the current posting to the end.
            --num_lists_remaining;
            pair<uint32_t, int> curr = lists_curr_postings[i];
            for(j = i; j < num_lists_remaining; ++j) {
              lists_curr_postings[j] = lists_curr_postings[j+1];
            }
            lists_curr_postings[num_lists_remaining] = curr;
            --i;
          }
        }

        // Decide whether docID makes it into the top-k.
        if (total_num_results < kMaxNumResults) {
          // We insert a document if we don't have k documents yet.
          results[total_num_results] = make_pair(bm25_sum, pivot.first);
          push_heap(results, results + total_num_results + 1, ResultCompare());
        } else {
          if (bm25_sum > results->first) {
            // We insert a document only if it's score is greater than the minimum scoring document in the heap.
            pop_heap(results, results + kMaxNumResults, ResultCompare());
            results[kMaxNumResults - 1].first = bm25_sum;
            results[kMaxNumResults - 1].second = pivot.first;
            push_heap(results, results + kMaxNumResults, ResultCompare());

            // Update the threshold.
            threshold = results->first;
          }
        }
        ++total_num_results;
      } else {
        // We don't have enough weight on the pivot yet. We know this is true when the docID from the first list != docID at the pivot.
        // There are two simple strategies that we can employ:
        // * Advance any one list before the pivot (just choose the first list). This is the original WAND algorithm.
        // * Advance all lists before the pivot (saves a few sorting operations at the cost of less list skipping). This is the mWAND algorithm.
        //   Main point is that index accesses are cheaper when the index is in main memory, so we try to do less list pointer sorting operations instead.
        // In both strategies, we advance the list pointer(s) at least to the pivot docID.
        if (kMWand) {
          for (i = 0; i < num_lists_remaining; ++i) {
            // Advance list pointer.
            if ((lists_curr_postings[i].first = index_reader_.NextGEQ(list_data_pointers[lists_curr_postings[i].second], pivot.first)) == numeric_limits<uint32_t>::max()) {
              // Compact the array. Move the current posting to the end.
              --num_lists_remaining;
              pair<uint32_t, int> curr = lists_curr_postings[i];
              for (j = i; j < num_lists_remaining; ++j) {
                lists_curr_postings[j] = lists_curr_postings[j + 1];
              }
              lists_curr_postings[num_lists_remaining] = curr;
              --i;
            }
          }
        } else {
          if ((lists_curr_postings[0].first = index_reader_.NextGEQ(list_data_pointers[lists_curr_postings[0].second], pivot.first)) == numeric_limits<uint32_t>::max()) {
            // Just swap the current posting with the one at the end of the array.
            // We'll be sorting at the start of the loop, so we don't need to compact and keep the order of the postings.
            --num_lists_remaining;
            pair<uint32_t, int> curr = lists_curr_postings[0];
            lists_curr_postings[0] = lists_curr_postings[num_lists_remaining];
            lists_curr_postings[num_lists_remaining] = curr;
          }
        }
      }
    }
  }

  // Sort top-k results in descending order by document score.
  sort(results, results + min(kMaxNumResults, total_num_results), ResultCompare());

  *num_results = min(total_num_results, kMaxNumResults);
  for (int i = 0; i < num_query_terms; ++i) {
    index_reader_.CloseList(list_data_pointers[i]);
  }
  return total_num_results;
}


// TODO:
// Difference between MaxScore and WAND is that once the threshold is sufficient enough, MaxScore will ignore the rest of the new docIDs in lists
// whose upperbounds indicate that they can't make it into the top-k.
//
// WAND is more akin to AND mode, since we move all list pointers to a common docID before scoring a document (unless we skip it); the difference being that we don't require the query terms to
// appear in all docIDs. Here, we skip scoring whole docIDs.
//
// MaxScore is more akin to OR mode, since we score a posting as soon as we reach it in the postings list (with the exception that we are able to skip scoring some postings).
// Here, we skip scoring individual postings.
//
// Use the MaxScore and Two Level MaxScore algorithms.
int QueryProcessor::MergeListsMaxScore(LexiconData** query_term_data, int num_query_terms, Result* results, int* num_results, bool two_tiered) {
  // Constraints on the type of index we expect.
  assert(index_layered_);
  assert(index_overlapping_layers_);
  assert(index_num_layers_ == 2);

  const int kMaxNumResults = *num_results;

  // Holds a pointer to the list for each corresponding query term.
  ListData* list_data_pointers[num_query_terms];  // Using a variable length array here.

  // For MaxScore to work correctly, need term upperbounds on the whole list.
  float list_thresholds[num_query_terms];  // Using a variable length array here.

  bool single_term_query = false;
  if (num_query_terms == 1) {
    single_term_query = true;
  }

  for (int i = 0; i < num_query_terms; ++i) {
    // Open the first layer (the top docs).
    list_data_pointers[i] = index_reader_.OpenList(*query_term_data[i], 0, single_term_query);
    list_thresholds[i] = list_data_pointers[i]->score_threshold();
#ifdef IRTK_DEBUG
    cout << "Top Docs Layer for '" << string(query_term_data[i]->term(), query_term_data[i]->term_len())
        << "', Layer Num: 0, Score Threshold: " << list_data_pointers[i]->score_threshold()
        << ", Num Docs: " << list_data_pointers[i]->num_docs() << endl;
#endif
  }

  int total_num_results = 0;
  if (num_query_terms == 1) {
    // Do standard DAAT OR mode processing, since WAND won't help.
    if (index_layered_ && query_term_data[0]->num_layers() == 2) {
      // We have two layers, so let's run the standard DAAT OR on the first layer only.
      // If there are k results, we can stop; otherwise rerun the query on the second layer.
      total_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
      if (total_num_results < kMaxNumResults) {
        index_reader_.CloseList(list_data_pointers[0]);
        list_data_pointers[0] = index_reader_.OpenList(*query_term_data[0], query_term_data[0]->num_layers() - 1, single_term_query);
        total_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
      }
    } else {
      // There is only one layer, run the query on it.
      total_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
    }
  } else {
    /*
     * We can estimate the threshold after processing the top docs lists in OR mode, but we can't decrease the upperbounds on the 2nd layers
     * because this will result in many of our high scoring documents to be skipped from the 2nd layers (including the ones from the top docs lists).
     *
     * TODO: What are some ways of decreasing the upperbound on the 2nd layers...?
     */

    float threshold = 0;
    if (two_tiered) {
      // It's possible that after processing the top docs, there is an unresolved docID (only present in some of the top docs lists, but not in others)
      // that could have a score higher than the top-k threshold we derive here.
      // For this reason, we can't early terminate here if we get k results.
      int top_docs_num_results = MergeLists(list_data_pointers, num_query_terms, results, kMaxNumResults);
#ifdef IRTK_DEBUG
      cout << "Num results from top docs lists: " << top_docs_num_results << endl;
#endif

      // The k-th score in the heap we get from the union of the top docs layers is our starting threshold.
      // It is a lower bound for the score necessary for a new docID to make it into our top-k.
      // The threshold is 0 if we didn't get k results from the top docs layers, meaning any docID can make it into the top-k.
      threshold = (top_docs_num_results >= kMaxNumResults) ? results[kMaxNumResults - 1].first : 0;
#ifdef IRTK_DEBUG
      cout << "Threshold from top docs lists: " << threshold << endl;
#endif
    }

    // We have to make sure that the layers are overlapping. So we'll be traversing the top-docs twice (in the second overlapping layer).
    // This is necessary because we're not using accumulators for the top-docs lists. It's only an approximate lower bound score on the top docIDs, since
    // the docID may be present in other lists, that did not make it into the top-docs.
    for (int i = 0; i < num_query_terms; ++i) {
      if (query_term_data[i]->num_layers() == 1) {
        // For a single layered list, we'll have to traverse it again.
        list_data_pointers[i]->ResetList(single_term_query);
      } else {
        // For a dual layered list, we close the first layer and open the second layer.
        index_reader_.CloseList(list_data_pointers[i]);
        list_data_pointers[i] = index_reader_.OpenList(*query_term_data[i], query_term_data[i]->num_layers() - 1, single_term_query);
      }

#ifdef IRTK_DEBUG
      cout << "Overlapping Layer for '" << string(query_term_data[i]->term(), query_term_data[i]->term_len())
          << "', Layer Num: " << (query_term_data[i]->num_layers() - 1)
          << ", Score Threshold: " << list_data_pointers[i]->score_threshold()
          << ", Num Docs: " << list_data_pointers[i]->num_docs() << endl;
#endif
    }

    // BM25 parameters: see 'http://en.wikipedia.org/wiki/Okapi_BM25'.
    const float kBm25K1 =  2.0;  // k1
    const float kBm25B = 0.75;   // b

    // We can precompute a few of the BM25 values here.
    const float kBm25NumeratorMul = kBm25K1 + 1;
    const float kBm25DenominatorAdd = kBm25K1 * (1 - kBm25B);
    const float kBm25DenominatorDocLenMul = kBm25K1 * kBm25B / collection_average_doc_len_;

    // BM25 components.
    float bm25_sum;  // The BM25 sum for the current document we're processing in the intersection.
    int doc_len;
    uint32_t f_d_t;

    // Compute the inverse document frequency component. It is not document dependent, so we can compute it just once for each list.
    float idf_t[num_query_terms];  // Using a variable length array here.
    int num_docs_t;
    for (int i = 0; i < num_query_terms; ++i) {
      num_docs_t = list_data_pointers[i]->num_docs_complete_list();
      idf_t[i] = log10(1 + (collection_total_num_docs_ - num_docs_t + 0.5) / (num_docs_t + 0.5));
    }

    // We use this to get the next lowest docID from all the lists.
    uint32_t lists_curr_postings[num_query_terms];  // Using a variable length array here.
    for (int i = 0; i < num_query_terms; ++i) {
      lists_curr_postings[i] = index_reader_.NextGEQ(list_data_pointers[i], 0);
    }

    pair<float, int> list_upperbounds[num_query_terms];  // Using a variable length array here.
    int num_lists_remaining = 0;  // The number of lists with postings remaining.
    for (int i = 0; i < num_query_terms; ++i) {
      if (lists_curr_postings[i] != numeric_limits<uint32_t>::max()) {
        list_upperbounds[num_lists_remaining++] = make_pair(list_thresholds[i], i);
      }
    }

    sort(list_upperbounds, list_upperbounds + num_lists_remaining, greater<pair<float, int> > ());

    // Precalculate the upperbounds for all possibilities.
    for (int i = num_lists_remaining - 2; i >= 0; --i) {
      list_upperbounds[i].first += list_upperbounds[i + 1].first;
    }

    /*// When a list has no more postings remaining, we can remove it right away, or wait until we iterated through the rest of the lists,
    // and remove any that have no more postings remaining. Removing them after iterating through all lists required an additional if statement.
    // What's odd is that when we remove the threshold checks (so that we can no longer early terminate), setting this option to 'false'
    // performs about 2ms faster (we wouldn't expect it to because of the extra if statement). However, when the threshold checks are in place,
    // setting this option to 'true' performs slightly faster (1-2ms). As far as I can tell, both do the same thing.
    const bool kCompactArrayRightAway = false;*/

    // When 'true', enables the use of embedded list score information to provide further efficiency gains
    // through better list skipping and less scoring computations.
    const bool kScoreSkipping = true;

    /*
     * For score skipping:
     *
     * TODO: BLOCKS CAN HAVE MULTIPLE LISTS ---- Need a structure where the scores are per list.
     * Maybe put it in an external file and load it into memory? The Lexicon can then have a pointer. Then it's easy to also extend this to per chunk.
     *
     * The 2nd layer will have lower threshold set than some of the blocks withing --- but this is because it's overlapping!
     * This is fine though, because we always use the 1st layer as the actual threshold.
     * What really is the problem, is that a block can have multiple lists and only one score upperbound --- need to fix this!!
     *
     *
     * Query: beneficiaries insurance irs life term
     * TODO: total_num_results counter overflows -- and we get a seg fault. WHY?
     */


    int i, j;
    int curr_list_idx;
    pair<float, int>* top;
    uint32_t curr_doc_id;  // Current docID we're processing the score for.
    /*bool compact_upperbounds = false;*/

    while (num_lists_remaining) {
      top = &list_upperbounds[0];
      /*if (kScoreSkipping && threshold > list_upperbounds[1].first) {
//        cout << "score skipping --- moving first list" << endl;TODO

        // Only the first (highest scoring) list can contain a docID that can still make it into the top-k,
        // so we move the first list to the first docID that has an upperbound that will allow it to make it into the top-k.
        if ((lists_curr_postings[0] = index_reader_.NextGEQScore(list_data_pointers[list_upperbounds[0].second], threshold - list_upperbounds[1].first)) == numeric_limits<uint32_t>::max()) {
          // Can early terminate at this point.
          break;
        }

        cout << "Skipping to docID: " << lists_curr_postings[0] << endl;

      } else {*/
        // Find the lowest docID that can still possibly make it into the top-k (while being able to make it into the top-k).
        for (i = 1; i < num_lists_remaining; ++i) {
          curr_list_idx = list_upperbounds[i].second;
          if (threshold > list_upperbounds[i].first) {
            break;
          }

          if (lists_curr_postings[curr_list_idx] < lists_curr_postings[top->second]) {
            top = &list_upperbounds[i];
          }
        }
//      }

      // Check if we can early terminate. This might happen only after we have finished traversing at least one list.
      // This is because our upperbounds don't decrease unless we are totally finished traversing one list.
      // Must check this since we initialize top to point to the first element in the list upperbounds array by default.
      if (threshold > list_upperbounds[0].first) {
        break;
      }

      // At this point, 'curr_doc_id' can either not be able to exceed the threshold score, or it can be the max possible docID sentinel value.
      curr_doc_id = lists_curr_postings[top->second];

      // We score a docID fully here, making any necessary lookups right away into other lists.
      // Disadvantage with this approach is that you'll be doing a NextGEQ() more than once for some lists on the same docID.
      bm25_sum = 0;
      for (i = 0; i < num_lists_remaining; ++i) {
        curr_list_idx = list_upperbounds[i].second;

        // Check if we can early terminate the scoring of this particular docID.
        if (threshold > bm25_sum + list_upperbounds[i].first) {
          break;
        }

        // Move to the curr docID we're scoring.
        lists_curr_postings[curr_list_idx] = index_reader_.NextGEQ(list_data_pointers[curr_list_idx], curr_doc_id);


        ///////////////////TODO: DEBUG
        if (curr_doc_id == 25170420) {
          cout << "Found it: " << curr_doc_id << endl;
        }
        ///////////////////////////////


        // Use the tighter score bound we have on the current list to see if we can early terminate the scoring of this particular docID.
        // TODO: To avoid the (i == num_lists_remaining - 1) test, can insert a dummy list with upperbound 0.
        if (kScoreSkipping && threshold > bm25_sum + index_reader_.GetBlockScoreBound(list_data_pointers[curr_list_idx]) + ((i == num_lists_remaining - 1) ? 0 : list_upperbounds[i + 1].first)) {

          cout << "Curr block bound for docID: " << curr_doc_id << ", is: "<< index_reader_.GetBlockScoreBound(list_data_pointers[curr_list_idx]) << endl;
          cout << "List with # docs: " << list_data_pointers[curr_list_idx]->num_docs() << endl;
          cout << "threshold: " << threshold << endl;
          cout << "Remaining upperbounds: " << ((i == num_lists_remaining - 1) ? 0 : list_upperbounds[i + 1].first) << endl;
          // TODO: Maybe print what scores this upperbound conists of.

          --num_lists_remaining;
          float curr_list_upperbound = list_thresholds[curr_list_idx];

          // Compact the list upperbounds array.
          for (j = i; j < num_lists_remaining; ++j) {
            list_upperbounds[j] = list_upperbounds[j + 1];
          }

          // Recalculate the list upperbounds. Note that we only need to recalculate those entries less than i.
          for (j = 0; j < i; ++j) {
            list_upperbounds[j].first -= curr_list_upperbound;
          }
          break;
        }

        if (lists_curr_postings[curr_list_idx] == curr_doc_id) {
          // Compute BM25 score from frequencies.
          f_d_t = index_reader_.GetFreq(list_data_pointers[curr_list_idx], lists_curr_postings[curr_list_idx]);
          doc_len = index_reader_.GetDocLen(lists_curr_postings[curr_list_idx]);
          bm25_sum += idf_t[curr_list_idx] * (f_d_t * kBm25NumeratorMul) / (f_d_t + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len);

          ++num_postings_scored_;

          // Can now move the list pointer further.
          lists_curr_postings[curr_list_idx] = index_reader_.NextGEQ(list_data_pointers[curr_list_idx], lists_curr_postings[curr_list_idx] + 1);
        }

        if (lists_curr_postings[curr_list_idx] == numeric_limits<uint32_t>::max()) {
          /*if (kCompactArrayRightAway) {*/
            --num_lists_remaining;
            float curr_list_upperbound = list_thresholds[curr_list_idx];

            // Compact the list upperbounds array.
            for (j = i; j < num_lists_remaining; ++j) {
              list_upperbounds[j] = list_upperbounds[j + 1];
            }

            // Recalculate the list upperbounds. Note that we only need to recalculate those entries less than i.
            for (j = 0; j < i; ++j) {
              list_upperbounds[j].first -= curr_list_upperbound;
            }
            --i;
          /*} else {
            compact_upperbounds = true;
          }*/
        }
      }

      // Need to keep track of the top-k documents.
      if (total_num_results < kMaxNumResults) {
        // We insert a document if we don't have k documents yet.
        results[total_num_results] = make_pair(bm25_sum, curr_doc_id);
        push_heap(results, results + total_num_results + 1, ResultCompare());
      } else {
        if (bm25_sum > results->first) {
          // We insert a document only if it's score is greater than the minimum scoring document in the heap.
          pop_heap(results, results + kMaxNumResults, ResultCompare());
          results[kMaxNumResults - 1].first = bm25_sum;
          results[kMaxNumResults - 1].second = curr_doc_id;
          push_heap(results, results + kMaxNumResults, ResultCompare());

          // Update the threshold.
          threshold = results->first;
        }
      }
      ++total_num_results;

      /*if (!kCompactArrayRightAway) {
        if (compact_upperbounds) {
          int num_lists = num_lists_remaining;
          num_lists_remaining = 0;
          for (i = 0; i < num_lists; ++i) {
            curr_list_idx = list_upperbounds[i].second;
            if (lists_curr_postings[curr_list_idx] != numeric_limits<uint32_t>::max()) {
              list_upperbounds[num_lists_remaining++] = make_pair(list_thresholds[curr_list_idx], curr_list_idx);
            }
          }

          sort(list_upperbounds, list_upperbounds + num_lists_remaining, greater<pair<float, int> > ());

          // Precalculate the upperbounds for all possibilities.
          for (i = num_lists_remaining - 2; i >= 0; --i) {
            list_upperbounds[i].first += list_upperbounds[i + 1].first;
          }

          compact_upperbounds = false;
        }
      }*/
    }
  }

  // Sort top-k results in descending order by document score.
  sort(results, results + min(kMaxNumResults, total_num_results), ResultCompare());

  *num_results = min(total_num_results, kMaxNumResults);
  for (int i = 0; i < num_query_terms; ++i) {
    index_reader_.CloseList(list_data_pointers[i]);
  }
  return total_num_results;
}

int QueryProcessor::IntersectLists(ListData** lists, int num_lists, Result* results, int num_results) {
  return IntersectLists(NULL, 0, lists, num_lists, results, num_results);
}

// Returns the total number of document results found in the intersection.
int QueryProcessor::IntersectLists(ListData** merge_lists, int num_merge_lists, ListData** lists, int num_lists, Result* results, int num_results) {
  // We have a choice of whether to use a heap (push() / pop() an array) or just search through an array to replace low scoring results
  // and finally sorting it before returning the top-k results in sorted order.
  // For k = 10 results, an array performs only slightly better than a heap. As k increases above 10, heap should be faster.
  // In the general case, a heap should be used (unless k is less than 10), so this option should be 'false'.
  const bool kUseArrayInsteadOfHeap = false;

  int total_num_results = 0;

  // For the array instead of heap top-k technique.
  float curr_min_doc_score;
  Result* min_scoring_result = NULL;

  // BM25 parameters: see 'http://en.wikipedia.org/wiki/Okapi_BM25'.
  const float kBm25K1 =  2.0;  // k1
  const float kBm25B = 0.75;   // b

  // We can precompute a few of the BM25 values here.
  const float kBm25NumeratorMul = kBm25K1 + 1;
  const float kBm25DenominatorAdd = kBm25K1 * (1 - kBm25B);
  const float kBm25DenominatorDocLenMul = kBm25K1 * kBm25B / collection_average_doc_len_;

  // BM25 components.
  float bm25_sum;  // The BM25 sum for the current document we're processing in the intersection.
  int doc_len;
  uint32_t f_d_t;

  uint32_t did = 0;
  uint32_t d;
  int i;  // Index for various loops.

  // Compute the inverse document frequency component. It is not document dependent, so we can compute it just once for each list.
  float idf_t[num_lists];  // Using a variable length array here.
  int num_docs_t;
  for (i = 0; i < num_lists; ++i) {
    num_docs_t = lists[i]->num_docs_complete_list();
    idf_t[i] = log10(1 + (collection_total_num_docs_ - num_docs_t + 0.5) / (num_docs_t + 0.5));
  }

  // Necessary for the merge lists.
  // TODO: Can also try the heap based method here. Can select between heap and array method based on 'num_merge_lists'.
  uint32_t min_doc_id;

  while (did < numeric_limits<uint32_t>::max()) {
    if (merge_lists != NULL) { // For the lists which we are merging.
      // This will select the lowest docID (ignoring duplicates among the merge lists and any docIDs we have skipped past through AND mode operation).
      min_doc_id = numeric_limits<uint32_t>::max();
      for (i = 0; i < num_merge_lists; ++i) {
        if ((d = index_reader_.NextGEQ(merge_lists[i], did)) < min_doc_id) {
          min_doc_id = d;
        }
      }

      assert(min_doc_id >= did);

      did = min_doc_id;
      i = 0;
    } else {
      // Get next element from shortest list.
      did = index_reader_.NextGEQ(lists[0], did);
      i = 1;
    }

    if (did == numeric_limits<uint32_t>::max())
      break;

    d = did;

    // Try to find entries with same docID in other lists.
    for (; (i < num_lists) && ((d = index_reader_.NextGEQ(lists[i], did)) == did); ++i) {
      continue;
    }

    if (d > did) {
      // Not in intersection.
      did = d;
    } else {
      assert(d == did);

      // Compute BM25 score from frequencies.
      bm25_sum = 0;
      for (i = 0; i < num_lists; ++i) {
        f_d_t = index_reader_.GetFreq(lists[i], did);
        doc_len = index_reader_.GetDocLen(did);
        bm25_sum += idf_t[i] * (f_d_t * kBm25NumeratorMul) / (f_d_t + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len);

//        //////////////////////TODO: DEBUG
//        if(did == 3122798) {
//          cout << "Partial score for list with idf: " << idf_t[i] << ", is: " << (idf_t[i] * (f_d_t * kBm25NumeratorMul) / (f_d_t + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len)) << endl;
//        }
//        ////////////////////

        //////////////////////////////TODO
        //        cout << "RESULT: " << "docID: " << did << ", score: " << bm25_sum << ", num_docs_complete_list: " << lists[i]->num_docs_complete_list() << ", f_d_t[i]: " << f_d_t[i] << endl;
        //////////////////////////////
      }

      if (kUseArrayInsteadOfHeap) {
        // Use an array to maintain the top-k documents.
        if (total_num_results < num_results) {
          results[total_num_results] = make_pair(bm25_sum, did);
          if (min_scoring_result == NULL || bm25_sum < min_scoring_result->first)
            min_scoring_result = results + total_num_results;
        } else {
          if (bm25_sum > min_scoring_result->first) {
            // TODO
            // Since the heap method discriminates against lower docIDs if the score is the same, we should do the same here.
            // The heap is a min heap, where the min document gets removed and a lower docID, given the same score, will cause the document with the
            // lower docID to wind up on top of the heap.

            // Here we actually discriminate against the lower scoring docIDs. If we insert multiple docIDs with the same score before we get k results,
            // any other (higher) docIDs with the same score will be rejected.
            // However, when we insert a new docID, we will remove the first document
            // found that has the same minimum score, even if there are multiple documents with that same minimum score; that first document will have a lower docID
            // since it was inserted first.

            // Solution could be to insert documents with the same min score.

            // Replace the min scoring result with the current (higher scoring) result.
            min_scoring_result->first = bm25_sum;
            min_scoring_result->second = did;

            // Find the new min scoring document.
            curr_min_doc_score = numeric_limits<float>::max();
            for (i = 0; i < num_results; ++i) {
              if (results[i].first < curr_min_doc_score) {
                curr_min_doc_score = results[i].first;
                min_scoring_result = results + i;
              }
            }
          }
        }
      } else {
        // Use a heap to maintain the top-k documents. This has to be a min heap,
        // where the lowest scoring document is on top, so that we can easily pop it,
        // and push a higher scoring document if need be.
        if (total_num_results < num_results) {
          // We insert a document if we don't have k documents yet.
          results[total_num_results] = make_pair(bm25_sum, did);
          push_heap(results, results + total_num_results + 1, ResultCompare());
        } else {
          if (bm25_sum > results->first) {
            // We insert a document only if it's score is greater than the minimum scoring document in the heap.
            pop_heap(results, results + num_results, ResultCompare());
            results[num_results - 1].first = bm25_sum;
            results[num_results - 1].second = did;
            push_heap(results, results + num_results, ResultCompare());
          }
        }
      }

      ++total_num_results;
      ++did;  // Search for next docID.
    }
  }

  // Sort top-k results in descending order by document score.
  sort(results, results + min(num_results, total_num_results), ResultCompare());

  return total_num_results;
}

// Processes queries in AND mode.
// For each docID that makes it into the top-k, we copy its positions list into a temporary memory location (passed in)
// and store a pointer to the positions as a tuple with the docID, the score, and the positions pointer.
// Returns the total number of document results found in the intersection.
int QueryProcessor::IntersectListsTopPositions(ListData** lists, int num_lists, Result* results, int num_results) {
  const int kNumLists = num_lists;                              // The number of lists we traverse.
  const int kMaxNumResults = num_results;                       // The maximum number of results we have keep in main memory at any time.
  const int kMaxPositions = MAX_FREQUENCY_PROPERTIES;           // The maximum number of positions for a docID in any list.
  const int kResultPositionStride = kMaxPositions + 1;          // For each result, per list, we store all the positions, plus an integer
                                                                // indicating the number of positions stored.
  const int kResultStride = kNumLists * kResultPositionStride;  // For each result, we have 'num_lists' worth of position information.

  // We will store position information for the top-k candidates in this array. The first k results will be stored sequentially, but afterwards
  // any result that gets pushed out of the top-k, will have it's positions replaced. We always store a pointer to the start of the positions
  // for each top-k result.
  uint32_t* position_pool = new uint32_t[kResultStride * kMaxNumResults];

  // The k temporary docID, score, and position pointer tuples, with a score comparator to maintain the top-k results.
  ResultPositionTuple* result_position_tuples = new ResultPositionTuple[kMaxNumResults];

  int total_num_results = 0;

  // BM25 parameters: see 'http://en.wikipedia.org/wiki/Okapi_BM25'.
  const float kBm25K1 =  2.0;  // k1
  const float kBm25B = 0.75;   // b

  // We can precompute a few of the BM25 values here.
  const float kBm25NumeratorMul = kBm25K1 + 1;
  const float kBm25DenominatorAdd = kBm25K1 * (1 - kBm25B);
  const float kBm25DenominatorDocLenMul = kBm25K1 * kBm25B / collection_average_doc_len_;

  // BM25 components.
  float bm25_sum;  // The BM25 sum for the current document we're processing in the intersection.
  int doc_len;
  uint32_t f_d_t[kNumLists];  // Using a variable length array here.
  const uint32_t* positions_d_t[kNumLists];  // Using a variable length array here.

  uint32_t did = 0;
  uint32_t d;
  int i, j, k;

  // Compute the inverse document frequency component. It is not document dependent, so we can compute it just once for each list.
  float idf_t[kNumLists];  // Using a variable length array here.
  int num_docs_t;
  for (i = 0; i < kNumLists; ++i) {
    num_docs_t = lists[i]->num_docs_complete_list();
    idf_t[i] = log10(1 + (collection_total_num_docs_ - num_docs_t + 0.5) / (num_docs_t + 0.5));
  }

  while (did < numeric_limits<uint32_t>::max()) {
    // Get next element from shortest list.
    if ((did = index_reader_.NextGEQ(lists[0], did)) == numeric_limits<uint32_t>::max())
      break;

    d = did;

    // Try to find entries with same docID in other lists.
    for (i = 1; (i < kNumLists) && ((d = index_reader_.NextGEQ(lists[i], did)) == did); ++i) {
      continue;
    }

    if (d > did) {
      // Not in intersection.
      did = d;
    } else {
      assert(d == did);

      // Compute BM25 score from frequencies.
      bm25_sum = 0;
      for (i = 0; i < kNumLists; ++i) {
        f_d_t[i] = index_reader_.GetFreq(lists[i], did);
        doc_len = index_reader_.GetDocLen(did);
        positions_d_t[i] = lists[i]->curr_block_decoder()->curr_chunk_decoder()->current_positions();
        bm25_sum += idf_t[i] * (f_d_t[i] * kBm25NumeratorMul) / (f_d_t[i] + kBm25DenominatorAdd + kBm25DenominatorDocLenMul * doc_len);
      }

      // Use a heap to maintain the top-k documents. This has to be a min heap,
      // where the lowest scoring document is on top, so that we can easily pop it,
      // and push a higher scoring document if need be.
      if (total_num_results < num_results) {
        // We insert a document if we don't have k documents yet.
        result_position_tuples[total_num_results].doc_id = did;
        result_position_tuples[total_num_results].score = bm25_sum;
        result_position_tuples[total_num_results].positions = &position_pool[total_num_results * kResultStride];
        for (i = 0; i < kNumLists; ++i) {
          result_position_tuples[total_num_results].positions[i * kResultPositionStride] = f_d_t[i];
          memcpy(&result_position_tuples[total_num_results].positions[(i * kResultPositionStride) + 1], positions_d_t[i], f_d_t[i]);
        }
        push_heap(result_position_tuples, result_position_tuples + total_num_results + 1);
      } else {
        if (bm25_sum > result_position_tuples[0].score) {
          // We insert a document only if it's score is greater than the minimum scoring document in the heap.
          pop_heap(result_position_tuples, result_position_tuples + num_results);
          result_position_tuples[num_results - 1].score = bm25_sum;
          result_position_tuples[num_results - 1].doc_id = did;
          // Replace the positions.
          for (i = 0; i < kNumLists; ++i) {
            result_position_tuples[num_results - 1].positions[i * kResultPositionStride] = f_d_t[i];
            memcpy(&result_position_tuples[total_num_results].positions[(i * kResultPositionStride) + 1], positions_d_t[i], f_d_t[i]);
          }
          push_heap(result_position_tuples, result_position_tuples + num_results);
        }
      }

      ++total_num_results;
      ++did;  // Search for next docID.
    }
  }

  // Utilize positions and prepare final result set.
  const int kNumReturnedResults = min(kMaxNumResults, total_num_results);
  for (i = 0; i < kNumReturnedResults; ++i) {
    for(j = 0; j < kNumLists; ++j) {
      const uint32_t* positions = &result_position_tuples[i].positions[j * kResultPositionStride];
      int num_positions = positions[0];
      ++positions;
      for (k = 0; k < num_positions; ++k) {
        cout << positions[k] << endl;
      }
    }

    results[i].first = result_position_tuples[i].score;
    results[i].second = result_position_tuples[i].doc_id;
  }

  delete[] result_position_tuples;
  delete[] position_pool;

  // Sort top-k results in descending order by document score.
  sort(results, results + kNumReturnedResults, ResultCompare());

  return total_num_results;
}

void QueryProcessor::ExecuteQuery(string query_line, int qid) {
  // All the words in the lexicon are lower case, so queries must be too, convert them to lower case.
  for (size_t i = 0; i < query_line.size(); i++) {
    if (isupper(query_line[i]))
      query_line[i] = tolower(query_line[i]);
  }

  if (query_mode_ == kBatch) {
    if (!silent_mode_)
      cout << "\nSearch: " << query_line << endl;
  }

  istringstream qss(query_line);
  vector<string> words;
  string term;
  while (qss >> term) {
    // Apply query time word stop list.
    // Remove words that appear in our stop list.
    if (!stop_words_.empty()) {
      if (stop_words_.find(term) == stop_words_.end()) {
        words.push_back(term);
      }
    } else {
      words.push_back(term);
    }
  }

  if (words.size() == 0) {
    if (!silent_mode_)
      cout << "Please enter a query.\n" << endl;
    return;
  }

  // Remove duplicate words, since there is no point in traversing lists for the same word multiple times.
  sort(words.begin(), words.end());
  words.erase(unique(words.begin(), words.end()), words.end());

  int num_query_terms = words.size();
  LexiconData* query_term_data[num_query_terms];  // Using a variable length array here.

  // For AND semantics, all query terms must exist in the lexicon for query processing to proceed.
  // For OR semantics, any of the query terms can be in the lexicon.
  enum ProcessingSemantics {
    kAnd, kOr, kUndefined
  };
  ProcessingSemantics processing_semantics;
  switch (query_algorithm_) {
    case kDaatAnd:
    case kDualLayeredOverlappingDaat:
    case kDualLayeredOverlappingMergeDaat:
      processing_semantics = kAnd;
      break;
    case kDaatOr:
    case kLayeredTaatOrEarlyTerminated:
    case kWand:
    case kDualLayeredWand:
    case kMaxScore:
    case kDualLayeredMaxScore:
      processing_semantics = kOr;
      break;
    default:
      processing_semantics = kUndefined;
      assert(false);
  }

  if (result_format_ == kCompare) {
    // Print the query.
    for (int i = 0; i < num_query_terms; ++i) {
      cout << words[i] << ((i != num_query_terms - 1) ? ' ' : '\n');
    }
  }

  int curr_query_term_num = 0;
  for (int i = 0; i < num_query_terms; ++i) {
    LexiconData* lex_data = index_reader_.lexicon().GetEntry(words[i].c_str(), words[i].length());
    if (lex_data != NULL)
      query_term_data[curr_query_term_num++] = lex_data;
  }

  if (processing_semantics == kOr) {
    num_query_terms = curr_query_term_num;
  }

  int results_size;
  int total_num_results;
  double query_elapsed_time;

  if (curr_query_term_num == num_query_terms) {
    results_size = max_num_results_;

    // These results are ranked from highest BM25 score to lowest.
    Result ranked_results[max_num_results_];  // Using a variable length array here.

    Timer query_time;  // Time how long it takes to answer a query.
    switch (query_algorithm_) {
      case kDaatAnd:
      case kDaatOr:
      case kDaatAndTopPositions:
        total_num_results = ProcessQuery(query_term_data, num_query_terms, ranked_results, &results_size);
        break;
      case kDualLayeredOverlappingDaat:
      case kDualLayeredOverlappingMergeDaat:
        total_num_results = ProcessLayeredQuery(query_term_data, num_query_terms, ranked_results, &results_size);
        break;
      case kLayeredTaatOrEarlyTerminated:
        total_num_results = ProcessLayeredTaatPrunedEarlyTerminatedQuery(query_term_data, num_query_terms, ranked_results, &results_size);
        break;
      case kWand:
        total_num_results = MergeListsWand(query_term_data, num_query_terms, ranked_results, &results_size, false);
        break;
      case kDualLayeredWand:
        total_num_results = MergeListsWand(query_term_data, num_query_terms, ranked_results, &results_size, true);
        break;
      case kMaxScore:
        total_num_results = MergeListsMaxScore(query_term_data, num_query_terms, ranked_results, &results_size, false);
        break;
      case kDualLayeredMaxScore:
        total_num_results = MergeListsMaxScore(query_term_data, num_query_terms, ranked_results, &results_size, true);
        break;
      default:
        total_num_results = 0;
        assert(false);
    }
    query_elapsed_time = query_time.GetElapsedTime();

    if (!warm_up_mode_) {
      total_querying_time_ += query_elapsed_time;
      ++total_num_queries_;
    }

    cout.setf(ios::fixed, ios::floatfield);
    cout.setf(ios::showpoint);

    if (result_format_ == kCompare) {
      cout << "num results: " << results_size << endl;
    }

    for (int i = 0; i < results_size; ++i) {
      switch (result_format_) {
        case kNormal:
          if (!silent_mode_)
            cout << setprecision(2) << setw(2) << "Score: " << ranked_results[i].first << "\tDocID: " << ranked_results[i].second << "\tURL: "
                << index_reader_.GetDocUrl(ranked_results[i].second) << "\n";
          break;
        case kTrec:
          if (!silent_mode_)
            cout << qid << '\t' << "Q0" << '\t' << index_reader_.GetDocUrl(ranked_results[i].second) << '\t' << i << '\t' << ranked_results[i].first << '\t'
                << "STANDARD" << "\n";
          break;
        case kCompare:
          cout << setprecision(2) << setw(2) << ranked_results[i].first << "\t" << ranked_results[i].second << "\n";
          break;
        case kDiscard:
          break;
        default:
          assert(false);
      }
    }
  } else {
    // One of the query terms did not exist in the lexicon.
    results_size = 0;
    total_num_results = 0;
    query_elapsed_time = 0;
  }

  if (result_format_ == kNormal)
    if (!silent_mode_)
      cout << "\nShowing " << results_size << " results out of " << total_num_results << ". (" << query_elapsed_time << " seconds)\n";
}

// We only count queries for which all terms are in the lexicon as part of the number of queries executed and the total elapsed querying time.
// This is because our query processor only supports AND queries; a query that contains terms which are not in the lexicon will just terminate
// with 0 results and 0 running time, so we ignore these for our benchmarking purposes.
void QueryProcessor::RunBatchQueries(istream& is, float percentage_test_queries) {
  vector<string> queries;
  string query_line;
  while (getline(is, query_line)) {
    size_t colon_pos = query_line.find(':');
    if (colon_pos != string::npos && colon_pos < (query_line.size() - 1)) {
      queries.push_back(query_line.substr(colon_pos + 1));
    } else {
      queries.push_back(query_line);
    }
  }

  if (percentage_test_queries != 1.0)
    random_shuffle(queries.begin(), queries.end());

  int num_test_queries = ceil(percentage_test_queries * queries.size());
  int num_warm_up_queries = queries.size() - num_test_queries;

  silent_mode_ = true;
  warm_up_mode_ = true;
  for (int i = 0; i < num_warm_up_queries; ++i) {
    ExecuteQuery(queries[i], 0);
  }

  index_reader_.ResetStats();

  silent_mode_ = ((percentage_test_queries != 1.0) ? false : true);
  warm_up_mode_ = false;
  for (int i = num_warm_up_queries; i < static_cast<int> (queries.size()); ++i) {
    ExecuteQuery(queries[i], 0);
  }
}

void QueryProcessor::LoadIndexProperties() {
  collection_total_num_docs_ = atol(index_reader_.meta_info().GetValue(meta_properties::kTotalNumDocs).c_str());
  if (collection_total_num_docs_ <= 0) {
    GetErrorLogger().Log("The '" + string(meta_properties::kTotalNumDocs) + "' value in the loaded index meta file seems to be incorrect.", false);
  }

  uint64_t collection_total_document_lengths = atol(index_reader_.meta_info().GetValue(meta_properties::kTotalDocumentLengths).c_str());
  if (collection_total_document_lengths <= 0) {
    GetErrorLogger().Log("The '" + string(meta_properties::kTotalDocumentLengths) + "' value in the loaded index meta file seems to be incorrect.", false);
  }

  if (collection_total_num_docs_ <= 0 || collection_total_document_lengths <= 0) {
    collection_average_doc_len_ = 1;
  } else {
    collection_average_doc_len_ = collection_total_document_lengths / collection_total_num_docs_;
  }

  if (!index_reader_.includes_positions()) {
    use_positions_ = false;
  }

  // Determine whether this index is layered and whether the index layers are overlapping.
  // From this info, we can determine the query processing mode.
  KeyValueStore::KeyValueResult<long int> layered_index_res = index_reader_.meta_info().GetNumericalValue(meta_properties::kLayeredIndex);
  KeyValueStore::KeyValueResult<long int> overlapping_layers_res = index_reader_.meta_info().GetNumericalValue(meta_properties::kOverlappingLayers);
  KeyValueStore::KeyValueResult<long int> num_layers_res = index_reader_.meta_info().GetNumericalValue(meta_properties::kNumLayers);

  // TODO:
  // If there are errors reading the values for these keys (most likely missing value), we assume they're false
  // (because that would require updating the index meta file generation in some places, which should be done eventually).
  index_layered_ = layered_index_res.error() ? false : layered_index_res.value_t();
  index_overlapping_layers_ = overlapping_layers_res.error() ? false : overlapping_layers_res.value_t();
  index_num_layers_ = num_layers_res.error() ? 1 : num_layers_res.value_t();

  bool inappropriate_algorithm = false;
  switch (query_algorithm_) {
    case kDefault:  // Choose a conservative algorithm based on the index properties.
      // Note that for a layered index with overlapping layers, we can do non-layered processing
      // by just opening the last layer from each list (which contains all the docIDs in the entire list).
      if (!index_layered_ || index_overlapping_layers_) {
        query_algorithm_ = kDaatAnd;  // TODO: Default should probably be an OR mode algorithm.
        break;
      }
      if (index_layered_ && !index_overlapping_layers_) {
        query_algorithm_ = kLayeredTaatOrEarlyTerminated;
        break;
      }
      break;
    case kDaatAnd:
    case kDaatOr:
    case kWand:  // TODO: For WAND, only need a single layered index, but need term upperbounds, which is not yet supported.
    case kDualLayeredWand:
    case kMaxScore:  // TODO: For MaxScore, only need a single layered index, but need term upperbounds, which is not yet supported.
    case kDualLayeredMaxScore:
    case kDaatAndTopPositions:
      if (index_layered_ && !index_overlapping_layers_) {
        inappropriate_algorithm = true;
      }
      break;
    case kDualLayeredOverlappingDaat:
    case kDualLayeredOverlappingMergeDaat:
      if (!index_layered_ || !index_overlapping_layers_ || index_num_layers_ != 2) {
        inappropriate_algorithm = true;
      }
      break;
    case kLayeredTaatOrEarlyTerminated:
      if (!index_layered_ || index_overlapping_layers_) {
        inappropriate_algorithm = true;
      }
      break;
    case kTaatOr:
      GetErrorLogger().Log("The selected query algorithm is not yet supported.", true);
      break;
    default:
      assert(false);
  }

  if (inappropriate_algorithm) {
    GetErrorLogger().Log("The selected query algorithm is not appropriate for this index type.", true);
  }
}

void QueryProcessor::PrintQueryingParameters() {
  cout << "collection_total_num_docs_: " << collection_total_num_docs_ << endl;
  cout << "collection_average_doc_len_: " << collection_average_doc_len_ << endl;
  cout << "Using positions: " << use_positions_ << endl;
  cout << endl;
}

const ExternalIndexReader* QueryProcessor::GetExternalIndexReader(QueryAlgorithm query_algorithm) const {
  switch (query_algorithm) {
    case kMaxScore:
    case kDualLayeredMaxScore:
      return new ExternalIndexReader("index.ext");
    default:
      return NULL;
  }
}
