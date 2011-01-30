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
// This class takes a standard index as input and splits the inverted lists into several pseudo lists, which we call layers here. Successive layers contain
// documents whose scores are lower than the previous layer(s). Each layer has a threshold score, which is the maximum partial BM25 score of a document in the
// list. Successive layers can also be overlapping, meaning that they will also contain the documents of all the previous layers.
//
// We assume here that any single inverted list can fit completely into main memory, mainly for simplicity. If not, we'd have to first split up each list,
// sort each piece individually by score, write out each piece to disk, then do a merge of the score sorted lists. Then, we wouldn't have to load the whole
// list into main memory in order to layer it, and it would be fully I/O efficient. However, in practice, loading the whole list into main memory is reasonable.
//==============================================================================================================================================================

#include "index_layerify.h"

#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <iostream>
#include <limits>
#include <fstream>
#include <sstream>

#include "coding_policy_helper.h"
#include "config_file_properties.h"
#include "configuration.h"
#include "external_index.h"
#include "globals.h"
#include "index_build.h"
#include "index_merge.h"
#include "index_reader.h"
#include "key_value_store.h"
#include "logger.h"
#include "meta_file_properties.h"
using namespace std;

/**************************************************************************************************************************************************************
 * LayeredIndexGenerator
 *
 **************************************************************************************************************************************************************/
LayeredIndexGenerator::LayeredIndexGenerator(const IndexFiles& input_index_files, const string& output_index_prefix) :
  output_index_files_(output_index_prefix),
  index_(NULL),
  external_index_builder_(NULL),
  index_builder_(NULL),
  includes_contexts_(true),
  includes_positions_(true),
  overlapping_layers_(false),
  num_layers_(0),
  doc_id_compressor_(CodingPolicy::kDocId),
  frequency_compressor_(CodingPolicy::kFrequency),
  position_compressor_(CodingPolicy::kPosition),
  block_header_compressor_(CodingPolicy::kBlockHeader),
  total_num_docs_(0),
  total_unique_num_docs_(0),
  total_document_lengths_(0),
  document_posting_count_(0),
  index_posting_count_(0),
  first_doc_id_in_index_(0),
  last_doc_id_in_index_(0) {
  external_index_builder_ = new ExternalIndexBuilder("index.ext");
  index_builder_ = new IndexBuilder(output_index_files_.lexicon_filename().c_str(), output_index_files_.index_filename().c_str(), block_header_compressor_,
                                    external_index_builder_);

  CacheManager* cache_policy = new MergingCachePolicy(input_index_files.index_filename().c_str());
  IndexReader* index_reader = new IndexReader(IndexReader::kMerge, *cache_policy, input_index_files.lexicon_filename().c_str(),
                                              input_index_files.document_map_filename().c_str(), input_index_files.meta_info_filename().c_str(), false);

  // Coding policy for the remapped index remains the same as that of the original index.
  coding_policy_helper::LoadPolicyAndCheck(doc_id_compressor_, index_reader->meta_info().GetValue(meta_properties::kIndexDocIdCoding), "docID");
  coding_policy_helper::LoadPolicyAndCheck(frequency_compressor_, index_reader->meta_info().GetValue(meta_properties::kIndexFrequencyCoding), "frequency");
  coding_policy_helper::LoadPolicyAndCheck(position_compressor_, index_reader->meta_info().GetValue(meta_properties::kIndexPositionCoding), "position");
  coding_policy_helper::LoadPolicyAndCheck(block_header_compressor_, index_reader->meta_info().GetValue(meta_properties::kIndexBlockHeaderCoding),
                                           "block header");

  if (!index_reader->includes_contexts())
    includes_contexts_ = false;

  if (!index_reader->includes_positions())
    includes_positions_ = false;

  // These must match in the layered index, except 'index_posting_count_' which is larger if the index layers are overlapping.
  total_num_docs_ = IndexConfiguration::GetResultValue(index_reader->meta_info().GetNumericalValue(meta_properties::kTotalNumDocs), false);
  total_unique_num_docs_ = IndexConfiguration::GetResultValue(index_reader->meta_info().GetNumericalValue(meta_properties::kTotalUniqueNumDocs), false);
  total_document_lengths_ = IndexConfiguration::GetResultValue(index_reader->meta_info().GetNumericalValue(meta_properties::kTotalDocumentLengths), false);
  document_posting_count_ = IndexConfiguration::GetResultValue(index_reader->meta_info().GetNumericalValue(meta_properties::kDocumentPostingCount), false);
  index_posting_count_ = IndexConfiguration::GetResultValue(index_reader->meta_info().GetNumericalValue(meta_properties::kIndexPostingCount), false);
  first_doc_id_in_index_ = IndexConfiguration::GetResultValue(index_reader->meta_info().GetNumericalValue(meta_properties::kFirstDocId), false);
  last_doc_id_in_index_ = IndexConfiguration::GetResultValue(index_reader->meta_info().GetNumericalValue(meta_properties::kLastDocId), false);

  index_ = new Index(cache_policy, index_reader);

  // Load the layering properties from the configuration file.
  overlapping_layers_ = Configuration::GetResultValue(Configuration::GetConfiguration().GetBooleanValue(config_properties::kOverlappingLayers));
  num_layers_ = Configuration::GetResultValue(Configuration::GetConfiguration().GetNumericalValue(config_properties::kNumLayers));
  if (num_layers_ <= 0 || num_layers_ > MAX_LIST_LAYERS) {
    Configuration::ErroneousValue(config_properties::kNumLayers, Stringify(num_layers_));
  }

  assert(includes_positions_ == false);  // TODO: We don't support layered indices with positions yet.
}

LayeredIndexGenerator::~LayeredIndexGenerator() {
  delete index_;
  delete index_builder_;
}

// TODO: For now, we assume the whole inverted list fits in main memory and we don't index positions.
// TODO: Computing BM25 scores during the various sorting stages is expensive. When sorting, we have to do n*log(n) comparisons and thus recompute the
//       BM25 score more than necessary. We can speedup by precomputing and storing the BM25 scores.
void LayeredIndexGenerator::CreateLayeredIndex() {
  GetDefaultLogger().Log("Creating layered index.", false);

  // Some static index layer properties.
  const int kLayerMinSize = CHUNK_SIZE;
  const int kMaxLayers = MAX_LIST_LAYERS;

  // TODO: Need a strategy that uses the IDF to split list into layers, so that only the top scoring documents are in the upper layers.
  // The parameters for the strategies should be included in config file, and which strategy to use by default.

  // We implement three different layer splitting strategies:
  // * Split layers by percentage.
  // * Split layers by percentage, limited by some max size for each layer.
  // * Split by exponentially increasing bucket sizes (based on the Anh/Moffat way of splitting, although they did this on a document level basis).
  // Each layer (except the last) will have at least 128 documents.
  enum LayerSplitMode {
    kPercentage, kPercentageFixedBounded, kExponentiallyIncreasing
  };
//  LayerSplitMode layer_splitting_strategy = kExponentiallyIncreasing;
  LayerSplitMode layer_splitting_strategy = kPercentageFixedBounded;

  // If we have overlapping layers, should the threshold score include the overlapping documents?
  // This should generally be set to 'false', since all layers will then have the same threshold stored,
  // so if any algorithm desires this effect, it can just use the first layer threshold as the threshold for all subsequent overlapping layers.
  const bool kOverlappingLayerThresholdIncludesAllDocs = false;

  // Some dynamic index layer properties.
  // TODO: Should be able to define in configuration file.
  int layer_percentages[] = { 5, 5, 10, 15, 25, 40, 0, 0 };
  int layer_max_sizes[] = { 1024, 8192, 0, 0, 0, 0, 0, 0 };  // Set the max number of postings in each layer, 0 means no limit. Used for 'kPercentageFixedBounded'.
  int layer_min_sizes[] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072 };  // Set the min number of postings in each layer, 0 means no limit. Used for 'kExponentiallyIncreasing'.

  // Test that the index layering properties make sense.
  if (num_layers_ > kMaxLayers) {
    GetErrorLogger().Log("Cannot make index with more layers than " + Stringify(kMaxLayers) + " layers.", true);
  }
  assert(sizeof(layer_percentages) >= (num_layers_ * sizeof(layer_percentages[0])));
  assert(sizeof(layer_max_sizes) >= (num_layers_ * sizeof(layer_max_sizes[0])));
  assert(sizeof(layer_min_sizes) >= (num_layers_ * sizeof(layer_min_sizes[0])));
  for (int i = 0; i < num_layers_; ++i) {
    assert(layer_percentages[i] >= 0);
    assert(layer_max_sizes[i] >= 0);
    assert(layer_min_sizes[i] >= 0);
  }

  while (index_->NextTerm()) {
#ifdef INDEX_LAYERIFY_DEBUG
    // Skip layering only to these lists for faster debugging.
    string curr_term = string(index_->curr_term(), index_->curr_term_len());
    if (!(curr_term == "beneficiaries" || curr_term == "insurance" || curr_term == "irs" || curr_term == "life" || curr_term == "hello" || curr_term == "world"
        || curr_term == "superior" || curr_term == "court" || curr_term == "king" || curr_term == "county" || curr_term == "divorce"))
      continue;
    cout << "Layering for inverted list: " << curr_term << endl;
#endif

    // TODO: It's better to reuse the buffer, and resize only when necessary.
    int num_docs_in_list = index_->curr_list_data()->num_docs();
    IndexEntry* index_entry_buffer = new IndexEntry[num_docs_in_list];
    int index_entry_offset = 0;

    while (index_->NextDocId()) {
      IndexEntry& curr_index_entry = index_entry_buffer[index_entry_offset];

      assert(index_entry_offset < num_docs_in_list);

      curr_index_entry.doc_id = index_->curr_doc_id();
      curr_index_entry.frequency = index_->curr_list_data()->GetFreq();

      ++index_entry_offset;
    }  // No more postings in the list.

    // Need the average document length for computing BM25 scores.
    long int total_document_lengths = IndexConfiguration::GetResultValue(index_->index_reader()->meta_info().GetNumericalValue(meta_properties::kTotalDocumentLengths), true);
    long int total_num_docs = IndexConfiguration::GetResultValue(index_->index_reader()->meta_info().GetNumericalValue(meta_properties::kTotalNumDocs), true);
    int average_doc_length = total_document_lengths / total_num_docs;

    // First, we sort by docID score.
    DocIdScoreComparison doc_id_score_comparator(index_->index_reader()->document_map(), num_docs_in_list, average_doc_length, total_num_docs);
#ifdef INDEX_LAYERIFY_DEBUG
    // For a particular term, print all the docIDs in the list and their partial BM25 scores, and group them in chunks.
    // This is before we sort the docIDs by score.
    if (curr_term == "superior" || curr_term == "divorce") {
      cout << "Printing docIDs and scores for list: " << curr_term << endl;
      cout << "total postings: " << index_entry_offset << endl;
      for (int i = 0; i < index_entry_offset; ++i) {
        if ((i & 127) == 0) {
          assert((i % 128) == 0);
          cout << "*New Chunk*" << endl;
        }
        cout << "docID: " << index_entry_buffer[i].doc_id << ", score: " << doc_id_score_comparator.Bm25Score(index_entry_buffer[i]) << endl;
      }
    }
#endif
    sort(index_entry_buffer, index_entry_buffer + index_entry_offset, doc_id_score_comparator);

    // For the exponentially increasing bucket size implementation.
    float base = pow(index_entry_offset, 1.0 / num_layers_);

    float list_score_threshold = 0;  // The upperbound score for the whole list.
    int total_num_postings = index_entry_offset;
    int num_postings_left = total_num_postings;
    int num_postings_curr_layer;
    for (int i = 0; i < num_layers_; ++i) {
      if (num_postings_left <= 0) {
        break;
      }

      switch (layer_splitting_strategy) {
        case kPercentage:
          num_postings_curr_layer = (layer_percentages[i] / 100.0) * total_num_postings;
          break;
        case kPercentageFixedBounded:
          num_postings_curr_layer = (layer_percentages[i] / 100.0) * total_num_postings;
          if (layer_max_sizes[i] != 0)  // A 0 means that the number of postings for this layer is not bounded.
            num_postings_curr_layer = min(num_postings_curr_layer, layer_max_sizes[i]);
          break;
        case kExponentiallyIncreasing:
          num_postings_curr_layer = (base - 1.0) * pow(base, i);
          if (layer_min_sizes[i] != 0)  // A 0 means that the number of postings for this layer is not bounded.
            num_postings_curr_layer = max(num_postings_curr_layer, layer_min_sizes[i]);
          break;
        default:
          assert(false);
          break;
      }

      // Potentially, due to the layering parameters, we will get more postings in the current layer than the total remaining postings,
      // and we have to normalize for that.
      num_postings_curr_layer = min(num_postings_curr_layer, num_postings_left);

      // Make each layer the minimum size (if there are enough postings remaining).
      if (num_postings_curr_layer < kLayerMinSize) {
        if (num_postings_left >= kLayerMinSize) {
          num_postings_curr_layer = kLayerMinSize;
        }
      }

      num_postings_left -= num_postings_curr_layer;

      // Make sure that if this is the last layer, it contains all the remaining postings.
      if (i == (num_layers_ - 1) && num_postings_left > 0) {
        num_postings_curr_layer += num_postings_left;
        num_postings_left = 0;
      }

      // We want to split so that scores in each layer are unique (i.e. the lowest scoring posting in one layer does not have the same score
      // as the highest scoring posting in the next layer).
      // This causes problems in early termination algorithms if not taken into account (the top-k documents returned will not be identical).
      // The solution to this problem is the following:
      // If the last posting of the current layer has the same score as the next n postings (which are in the next layer(s)),
      // we move those same scoring postings into the current layer.
      // If the next layer(s) now contain 0 documents, we push postings from layers further down into the upper layers.
      float curr_layer_threshold, next_layer_threshold;
      do {
        // If this is the layer layer, nothing needs to be done.
        if (i == (num_layers_ - 1) || num_postings_left <= 0)
          break;

        int curr_layer_threshold_idx = total_num_postings - num_postings_left - num_postings_curr_layer;
        int next_layer_threshold_idx = total_num_postings - num_postings_left;
        curr_layer_threshold = doc_id_score_comparator.Bm25Score(index_entry_buffer[curr_layer_threshold_idx]);
        next_layer_threshold = doc_id_score_comparator.Bm25Score(index_entry_buffer[next_layer_threshold_idx]);
        // The current layer threshold should always be greater than the next layer threshold.
        // We add postings to the current layer until the above is true.
        if (curr_layer_threshold <= next_layer_threshold) {
          ++num_postings_curr_layer;
          --num_postings_left;
        } else {
          break;
        }
      } while (true);

      assert(num_postings_curr_layer > 0);

      // Here we do the actual splitting of the layers.
      // TODO: Instead of resorting the whole buffer, it might be faster to sort only the 2nd layer by docID, and then do a merge of the layers.
      //       This would require a different DumpToIndex() method that is more incremental, because we can't do an in-place merge of the whole array
      //       (it would require an additional array).
      int curr_layer_start = total_num_postings - num_postings_left - num_postings_curr_layer;
      int layer_start = overlapping_layers_ ? 0 : curr_layer_start;
      float score_threshold = doc_id_score_comparator.Bm25Score(index_entry_buffer[curr_layer_start]);
      if (i == 0) {
        list_score_threshold = score_threshold;
      }
      if (kOverlappingLayerThresholdIncludesAllDocs) {
        score_threshold = list_score_threshold;
      }
      sort(index_entry_buffer + layer_start, index_entry_buffer + curr_layer_start + num_postings_curr_layer, IndexEntryDocIdComparison());
      DumpToIndex(doc_id_score_comparator, index_entry_buffer + layer_start, curr_layer_start + num_postings_curr_layer - layer_start, index_->curr_term(),
                  index_->curr_term_len());
      index_builder_->FinalizeLayer(score_threshold);  // Need to call this before writing out the next layer.
    }

    delete[] index_entry_buffer;
  }

  index_builder_->Finalize();

  WriteMetaFile(output_index_files_.meta_info_filename());

  GetDefaultLogger().Log("Finished creating layered index.", false);
}

// This dumps a single list into an index.
void LayeredIndexGenerator::DumpToIndex(const DocIdScoreComparison& doc_id_score_comparator, IndexEntry* index_entries, int num_index_entries,
                                        const char* curr_term, int curr_term_len) {
  // Since the following input arrays will be used as input to the various coding policies, and the coding policy might apply a blockwise coding compressor
  // (which would pad the array to the block size), the following rules apply:
  // For the docID and frequency arrays, the block size is expected to be the chunk size.
  // For the position and context arrays, the block size is expected to be a multiple of the maximum positions/contexts possible for a particular docID.
  // Some alternative designs would be to define a fixed maximum block size and make sure the arrays are properly sized for this maximum
  // (the position/context arrays in particular).
  // Another alternative is to make these arrays dynamically allocated.
  assert(doc_id_compressor_.block_size() == 0 || ChunkEncoder::kChunkSize == doc_id_compressor_.block_size());
  assert(frequency_compressor_.block_size() == 0 || ChunkEncoder::kChunkSize == frequency_compressor_.block_size());
  assert(position_compressor_.block_size() == 0 || (ChunkEncoder::kChunkSize * ChunkEncoder::kMaxProperties) % position_compressor_.block_size() == 0);

  uint32_t doc_ids[ChunkEncoder::kChunkSize];
  uint32_t frequencies[ChunkEncoder::kChunkSize];
  uint32_t positions[ChunkEncoder::kChunkSize * ChunkEncoder::kMaxProperties];
  unsigned char contexts[ChunkEncoder::kChunkSize * ChunkEncoder::kMaxProperties];

  uint32_t prev_chunk_last_doc_id = 0;
  uint32_t prev_doc_id = 0;

  int index_entries_offset = 0;
  while(index_entries_offset < num_index_entries) {
    int doc_ids_offset = 0;
    int properties_offset = 0;
    for(doc_ids_offset = 0; doc_ids_offset < ChunkEncoder::kChunkSize && index_entries_offset < num_index_entries; ++doc_ids_offset) {
      const IndexEntry& curr_index_entry = index_entries[index_entries_offset];

      doc_ids[doc_ids_offset] = curr_index_entry.doc_id - prev_doc_id;
      // Check for duplicate docIDs (when the difference between the 'curr_index_entry.doc_id' and 'prev_doc_id' is zero), which is considered a bug.
      // But since 'prev_doc_id' is initialized to 0, which is a valid doc,
      // we have a case where the 'curr_index_entry.doc_id' could start from 0, which is an exception to the rule.
      // Thus, if this is the first iteration and 'curr_index_entry.doc_id' is 0, it is an acceptable case.
      assert(doc_ids[doc_ids_offset] != 0 || (index_entries_offset == 0 && curr_index_entry.doc_id == 0));
      prev_doc_id = curr_index_entry.doc_id;

      frequencies[doc_ids_offset] = curr_index_entry.frequency;

      if (includes_positions_) {
        for (uint32_t j = 0; j < curr_index_entry.frequency; ++j) {
          positions[properties_offset++] = curr_index_entry.positions[j];
        }
      }

      ++index_entries_offset;
    }

    ChunkEncoder chunk(doc_ids, frequencies, (includes_positions_ ? positions : NULL), (includes_contexts_ ? contexts : NULL),
                                       doc_ids_offset, properties_offset, prev_chunk_last_doc_id, doc_id_compressor_, frequency_compressor_,
                                       position_compressor_);
    chunk.set_max_score(GetChunkMaxScore(doc_id_score_comparator, index_entries + (index_entries_offset - doc_ids_offset), doc_ids_offset));
    prev_chunk_last_doc_id = chunk.last_doc_id();
    index_builder_->Add(chunk, curr_term, curr_term_len);
  }
}

float LayeredIndexGenerator::GetChunkMaxScore(const DocIdScoreComparison& doc_id_score_comparator, IndexEntry* chunk_entries, int num_chunk_entries) {
  sort(chunk_entries, chunk_entries + num_chunk_entries, doc_id_score_comparator);
  return doc_id_score_comparator.Bm25Score(chunk_entries[0]);
}

void LayeredIndexGenerator::WriteMetaFile(const std::string& meta_filename) {
  KeyValueStore index_metafile;

  // TODO: Need to write the document offset to be used for the true docIDs in the index.
  // This will allow us to store smaller docIDs (for the non-gap-coded ones, anyway) resulting in better compression.

  // Index layer properties.
  // TODO: search project for WriteMetaFile, and add the layer stuff too!
  index_metafile.AddKeyValuePair(meta_properties::kLayeredIndex, Stringify(true));
  index_metafile.AddKeyValuePair(meta_properties::kNumLayers, Stringify(num_layers_));
  index_metafile.AddKeyValuePair(meta_properties::kOverlappingLayers, Stringify(overlapping_layers_));

  index_metafile.AddKeyValuePair(meta_properties::kIncludesPositions, Stringify(includes_positions_));
  index_metafile.AddKeyValuePair(meta_properties::kIncludesContexts, Stringify(includes_contexts_));
  index_metafile.AddKeyValuePair(meta_properties::kIndexDocIdCoding, IndexConfiguration::GetResultValue(index_->index_reader()->meta_info().GetStringValue(meta_properties::kIndexDocIdCoding), false));
  index_metafile.AddKeyValuePair(meta_properties::kIndexFrequencyCoding, IndexConfiguration::GetResultValue(index_->index_reader()->meta_info().GetStringValue(meta_properties::kIndexFrequencyCoding), false));
  index_metafile.AddKeyValuePair(meta_properties::kIndexPositionCoding, IndexConfiguration::GetResultValue(index_->index_reader()->meta_info().GetStringValue(meta_properties::kIndexPositionCoding), false));
  index_metafile.AddKeyValuePair(meta_properties::kIndexBlockHeaderCoding, IndexConfiguration::GetResultValue(index_->index_reader()->meta_info().GetStringValue(meta_properties::kIndexBlockHeaderCoding), false));

  index_metafile.AddKeyValuePair(meta_properties::kTotalNumChunks, Stringify(index_builder_->total_num_chunks()));
  index_metafile.AddKeyValuePair(meta_properties::kTotalNumPerTermBlocks, Stringify(index_builder_->total_num_per_term_blocks()));

  index_metafile.AddKeyValuePair(meta_properties::kTotalDocumentLengths, Stringify(total_document_lengths_));
  index_metafile.AddKeyValuePair(meta_properties::kTotalNumDocs, Stringify(total_num_docs_));
  index_metafile.AddKeyValuePair(meta_properties::kTotalUniqueNumDocs, Stringify(total_unique_num_docs_));

  index_metafile.AddKeyValuePair(meta_properties::kFirstDocId, Stringify(first_doc_id_in_index_));
  index_metafile.AddKeyValuePair(meta_properties::kLastDocId, Stringify(last_doc_id_in_index_));
  index_metafile.AddKeyValuePair(meta_properties::kDocumentPostingCount, Stringify(document_posting_count_));
  if ((!overlapping_layers_ && index_posting_count_ != index_builder_->posting_count()) ||
      (overlapping_layers_ && index_posting_count_ > index_builder_->posting_count())) {
    GetErrorLogger().Log("Inconsistency in the '" + string(meta_properties::kIndexPostingCount) + "' meta file property detected: "
        + "value from original index meta file doesn't add up to the value calculated by the index builder.", false);
  }
  index_metafile.AddKeyValuePair(meta_properties::kIndexPostingCount, Stringify(index_builder_->posting_count()));
  index_metafile.AddKeyValuePair(meta_properties::kNumUniqueTerms, Stringify(index_builder_->num_unique_terms()));

  index_metafile.AddKeyValuePair(meta_properties::kTotalHeaderBytes, Stringify(index_builder_->total_num_block_header_bytes()));
  index_metafile.AddKeyValuePair(meta_properties::kTotalDocIdBytes, Stringify(index_builder_->total_num_doc_ids_bytes()));
  index_metafile.AddKeyValuePair(meta_properties::kTotalFrequencyBytes, Stringify(index_builder_->total_num_frequency_bytes()));
  index_metafile.AddKeyValuePair(meta_properties::kTotalPositionBytes, Stringify(index_builder_->total_num_positions_bytes()));
  index_metafile.AddKeyValuePair(meta_properties::kTotalWastedBytes, Stringify(index_builder_->total_num_wasted_space_bytes()));

  index_metafile.WriteKeyValueStore(meta_filename.c_str());
}
