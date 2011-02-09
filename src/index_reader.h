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
// Responsible for providing low level index access operations.
//==============================================================================================================================================================

#ifndef INDEX_READER_H_
#define INDEX_READER_H_

// Enables debugging output for this module.
//#define INDEX_READER_DEBUG

#include <cassert>
#include <stdint.h>

#include <climits>

#ifdef INDEX_READER_DEBUG
#include <iostream>
#endif

#include "cache_manager.h"
#include "coding_policy.h"
#include "coding_policy_helper.h"
#include "document_map.h"
#include "external_index.h"
#include "index_configuration.h"
#include "index_layout_parameters.h"
#include "term_hash_table.h"

/**************************************************************************************************************************************************************
 * ChunkDecoder
 *
 * Responsible for decoding the data contained within a chunk and maintaining some state during list traversal.
 **************************************************************************************************************************************************************/
class ChunkDecoder {
public:
  ChunkDecoder();

  void InitChunk(int num_docs, const uint32_t* buffer);

  void DecodeDocIds(const CodingPolicy& doc_id_decompressor);

  void DecodeFrequencies(const CodingPolicy& frequency_decompressor);

  void DecodePositions(const CodingPolicy& position_decompressor);

  // Update the offset of the properties for the current document.
  // Requires 'frequencies_' to have been already decoded.
  void UpdatePropertiesOffset();

  uint32_t doc_id(int doc_id_idx) const {
    assert(doc_id_idx < num_docs_);
    return doc_ids_[doc_id_idx];
  }

  void set_doc_id(int doc_id_idx, uint32_t doc_id) {
    assert(doc_id_idx < num_docs_);
    doc_ids_[doc_id_idx] = doc_id;
  }

  uint32_t current_frequency() const {
    assert(curr_document_offset_ < num_docs_);
    assert(frequencies_[curr_document_offset_] != 0);
    return frequencies_[curr_document_offset_];
  }

  const uint32_t* current_positions() const {
    assert(curr_position_offset_ < num_positions_);
    return positions_ + curr_position_offset_;
  }

  int num_docs() const {
    return num_docs_;
  }

  int curr_document_offset() const {
    return curr_document_offset_;
  }

  void set_curr_document_offset(int doc_offset) {
    assert(doc_offset >= 0 && doc_offset >= curr_document_offset_);
    curr_document_offset_ = doc_offset;
  }

  int curr_position_offset() const {
    return curr_position_offset_;
  }

  uint32_t prev_decoded_doc_id() const {
    return prev_decoded_doc_id_;
  }

  void set_prev_decoded_doc_id(uint32_t decoded_doc_id) {
    prev_decoded_doc_id_ = decoded_doc_id;
  }

  void update_prev_decoded_doc_id(uint32_t doc_id_offset) {
    prev_decoded_doc_id_ += doc_id_offset;
  }

  bool decoded_doc_ids() const {
    return decoded_doc_ids_;
  }

  void set_decoded_doc_ids(bool decoded_doc_ids) {
    decoded_doc_ids_ = decoded_doc_ids;
  }

  bool decoded_properties() const {
    return decoded_properties_;
  }

  void set_decoded_properties(bool decoded_properties) {
    decoded_properties_ = decoded_properties;
  }

  float chunk_max_score() const {
    return chunk_max_score_;
  }

  void set_chunk_max_score(float chunk_max_score) {
    chunk_max_score_ = chunk_max_score;
  }

  static const int kChunkSize = CHUNK_SIZE;                    // The maximum number of documents that can be contained within a chunk.
  static const int kMaxProperties = MAX_FREQUENCY_PROPERTIES;  // The maximum number of properties per document.

private:
  int curr_document_offset_;              // The offset into the 'doc_ids_' and 'frequencies_' arrays of the current document we're processing.
                                          // This allows us to quickly get to the frequency information for the current document being processed
                                          // as well as speed up finding the next document to process in this chunk.
  uint32_t prev_decoded_doc_id_;          // The previously decoded doc id. This is used during query processing when decoding docID gaps.
  bool decoded_doc_ids_;                  // True when we have decoded the docIDs.
  bool decoded_properties_;               // True when we have decoded the document properties (that is, frequencies, positions, etc). Necessary because we
                                          // don't want to decode the frequencies/contexts/positions until we're certain we actually need them.
  float chunk_max_score_;                 // The maximum partial docID score contained within this chunk.
  int num_positions_;                     // Total number of decoded positions in this list.
  int prev_document_offset_;              // The position we last stopped at when updating the 'curr_position_offset_'.
  int curr_position_offset_;              // The offset into the 'positions_' array for the current document being processed.
  int num_docs_;                          // The number of documents in this chunk.
  const uint32_t* curr_buffer_position_;  // Pointer to the raw data of stuff we have to decode next.

  // These buffers are used for decompression of chunks.
  uint32_t doc_ids_[UncompressedOutBufferUpperbound(kChunkSize)];                     // Array of decompressed docIDs. If stored gap coded,
                                                                                      // the gaps are not decoded here, but rather during query processing.
  uint32_t frequencies_[UncompressedOutBufferUpperbound(kChunkSize)];                 // Array of decompressed frequencies.
  uint32_t positions_[UncompressedOutBufferUpperbound(kChunkSize * kMaxProperties)];  // Array of decomrpessed positions. The position gaps are not decoded
                                                                                      // here, but rather during query processing, if necessary at all.
};

/**************************************************************************************************************************************************************
 * BlockDecoder
 *
 * Responsible for decoding the block header from a particular block in the inverted list and maintaining some state during list traversal.
 **************************************************************************************************************************************************************/
class BlockDecoder {
public:
  BlockDecoder();

  void InitBlock(const CodingPolicy& block_header_decompressor, int starting_chunk, uint32_t* block_data);

  void DecodeHeader(const CodingPolicy& block_header_decompressor);

  // Returns the last fully decoded docID of the ('chunk_idx'+1)th chunk in the block.
  uint32_t chunk_last_doc_id(int chunk_idx) const {
    int idx = 2 * chunk_idx;
    assert(idx < (num_chunks_ * 2));
    return chunk_properties_[idx];
  }

  // Returns the size of the ('chunk_idx'+1)th chunk in the block in words (where a word is sizeof(uint32_t)).
  uint32_t chunk_size(int chunk_idx) const {
    int idx = 2 * (chunk_idx + 1) - 1;
    assert(idx < (num_chunks_ * 2));
    return chunk_properties_[idx];
  }

  // Returns the actual number of chunks stored in this block, that are actually part of the inverted list for this particular term.
  int num_actual_chunks() const {
    return num_chunks_ - starting_chunk_;
  }

  // Returns the maximum docID score contained within this block.
  float block_max_score() const {
    return block_max_score_;
  }

  // Sets the maximum docID score contained within this block.
  void set_block_max_score(float block_max_score) {
    block_max_score_ = block_max_score;
  }

  // Returns a pointer to the start of the next raw chunk that needs to be decoded.
  const uint32_t* curr_block_data() const {
    return curr_block_data_;
  }

  // Returns the index of the current chunk being traversed.
  int curr_chunk() const {
    return curr_chunk_;
  }

  // Updates 'curr_block_data_' to point to the next chunk in the raw data block for decoding.
  void advance_curr_chunk() {
    curr_block_data_ += chunk_size(curr_chunk_);
    ++curr_chunk_;
  }

  // The index of the starting chunk in this block for the inverted list for this particular term.
  int starting_chunk() const {
    return starting_chunk_;
  }

  // Returns the total number of chunks in this block.
  // This includes any chunks that are not part of the inverted list for this particular term.
  int num_chunks() const {
    return num_chunks_;
  }

  static const int kBlockSize = BLOCK_SIZE;  // The fixed size of each block, in bytes.

private:
  static const int kChunkSizeLowerBound = MIN_COMPRESSED_CHUNK_SIZE;

  // The upper bound on the number of chunk properties in a single block (with upperbounds for proper decompression by various coding policies),
  // calculated by getting the max number of chunks in a block and multiplying by 2 properties per chunk.
  static const int kChunkPropertiesDecompressedUpperbound = UncompressedOutBufferUpperbound(2 * (kBlockSize / kChunkSizeLowerBound));

  // For each chunk in the block corresponding to this current BlockDecoder, holds the last docIDs and sizes,
  // where the size is in words, and a word is sizeof(uint32_t). The last docID is always followed by the size, for every chunk, in this order.
  // This will require ~11KiB of memory. Alternative is to make dynamic allocations and resize if necessary.
  uint32_t chunk_properties_[kChunkPropertiesDecompressedUpperbound];

  float block_max_score_;      // The maximum docID score within this block.
  uint32_t* curr_block_data_;  // Points to the start of the next chunk to be decoded.
  int curr_chunk_;             // The current actual chunk number we're up to within a block.
  int starting_chunk_;         // The chunk number of the first chunk in the block of the associated list.
  int num_chunks_;             // The total number of chunks this block holds, regardless of which list it is.
};

/**************************************************************************************************************************************************************
 * ListData
 *
 * Implements the basic methods required for inverted list traversal.
 * Holds the current state of the list we're traversing (tracks the current position within an inverted list).
 **************************************************************************************************************************************************************/
class ListData {
public:
  enum RetrieveDataType {
    kDocId, kFrequency, kPosition
  };

  ListData(CacheManager& cache_manager, const CodingPolicy& doc_id_decompressor, const CodingPolicy& frequency_decompressor,
           const CodingPolicy& position_decompressor, const CodingPolicy& block_header_decompressor, int layer_num, uint32_t initial_block_num,
           uint32_t initial_chunk_num, int num_docs, int num_docs_complete_list, int num_chunks_last_block, int num_blocks, const uint32_t* last_doc_ids,
           float score_threshold, uint32_t external_index_offset, const ExternalIndexReader* external_index_reader, bool use_positions, bool single_term_query,
           bool block_skipping);
  ~ListData();

  // Resets the inverted list to it's initial state. After resetting, we can start decoding the list from the beginning again.
  void ResetList(bool single_term_query);

  // 'data_type' represents the type of index information we want to retrieve.
  // 'index_data' is an array of size 'index_data_size' where we will store the index data (up to the maximum size it can hold).
  // Returns the number of index data elements that have been stored into 'index_data'. A return of 0 indicates we retrieved all elements from the index.
  // Note: when retrieving position data, a return of -1 means that the supplied array needs to be larger to copy all positions for a single docID.
  // This function is designed to be called multiple times (with the same 'data_type') to get all the index data in chunks.
  int GetList(RetrieveDataType data_type, uint32_t* index_data, int index_data_size);

  // Loops over the list, decompressing the data type determined by 'data_type'. D-gap decoding does not take place, as we're only interested in decompression.
  // This is useful for benchmarking purposes or verifying that the list could be properly decoded.
  int LoopOverList(RetrieveDataType data_type);

  // Returns the next greater than or equal docID to that of 'doc_id'.
  uint32_t NextGEQ(uint32_t doc_id);

  // Returns the frequency for the current docID.
  uint32_t GetFreq();

  // Returns the number of per document properties (e.g. number of positions) for the current docID.
  uint32_t GetNumDocProperties();

  // Returns the next docID that has a score greater than 'min_score'. Skips ahead by blocks.
  uint32_t NextGreaterBlockScore(float min_score);

  // Returns the next docID that has a score greater than 'min_score'. Skips ahead by chunks.
  uint32_t NextGreaterChunkScore(float min_score);

  // Returns the upperbound score of the current block.
  float GetBlockScoreBound();

  // Returns the upperbound score of the current chunk.
  float GetChunkScoreBound();

  // Advances to the first block that can contain 'doc_id'. This employs block skipping (if enabled).
  void AdvanceBlock(uint32_t doc_id);

  // Advances to the next block.
  void AdvanceBlock();

  // Advances to the next chunk.
  void AdvanceChunk();

  const BlockDecoder& curr_block_decoder() {
    return curr_block_decoder_;
  }

  const ChunkDecoder& curr_chunk_decoder() {
    return curr_chunk_decoder_;
  }

  int layer_num() const {
    return layer_num_;
  }

  int num_docs() const {
    return num_docs_;
  }

  int num_docs_complete_list() const {
    return num_docs_complete_list_;
  }

  int num_chunks() const {
    return num_chunks_;
  }

  int num_blocks() const {
    return num_blocks_;
  }

  float score_threshold() const {
    return score_threshold_;
  }

  int term_num() const {
    return term_num_;
  }

  void set_term_num(int term_num) {
    term_num_ = term_num;
  }

  uint64_t cached_bytes_read() const {
    return cached_bytes_read_;
  }

  uint64_t disk_bytes_read() const {
    return disk_bytes_read_;
  }

  uint32_t num_blocks_skipped() const {
    return num_blocks_skipped_;
  }

  static const uint32_t kNoMoreDocs;  // Sentinel value indicating that there are no more docs available in the list.

private:
  void Init();

  void FreeQueuedBlocks();

  void SkipBlocks(int num_blocks, uint32_t initial_chunk_num);

  void BlockBinarySearch(uint32_t doc_id);

  void BlockSequentialSearch(uint32_t doc_id);

  void SkipToBlock(uint32_t skip_to_block_idx);

  bool initial_block() const {
    return (num_blocks_left_ == num_blocks_) ? true : false;
  }

  bool final_block() const {
    return (num_blocks_left_ == 1) ? true : false;
  }

  bool final_chunk() const {
    return (num_chunks_last_block_left_ == 1) ? true : false;
  }

  bool has_more() const {
    assert((num_chunks_last_block_left_ == 0) ? (num_blocks_left_ == 1) : true);
    return num_chunks_last_block_left_ != 0;
  }

  // Used about once per list.
  const int kNumLeftoverDocs;   // The uneven number of documents that spilled over into the last chunk of the list.
                                // (that is, they couldn't be evenly divided by the standard amount of documents in a full chunk).
  bool single_term_query_;      // A hint from an external source that allows us to optimize list traversal
                                // if we know that we'll never be doing any list skipping.
  int layer_num_;               // The layer this list is part of.
  int num_docs_;                // The total number of documents in this inverted list.
  int num_docs_complete_list_;  // The total number of documents in the complete inverted list (in case this is just one layer of a complete list).
                                // We use this amount to calculate the BM25 score, so that the layered and
                                // non-layered index approaches produce the same document scores.
  int num_chunks_;              // The total number of chunks we have in this inverted list.
  int num_chunks_last_block_;   // The number of chunks contained in the last block of this inverted list.
  uint32_t initial_chunk_num_;  // The initial chunk number this list starts in.
  uint32_t initial_block_num_;  // The initial block number this list starts in.
  bool first_block_loaded_;     // (block-level-skipping) Set to true when the first block in this inverted list was loaded. Otherwise, remains unused.

  // Used often (i.e. on every invocation of NextGEQ()).
  BlockDecoder curr_block_decoder_;  // The current block being processed in this inverted list.
  uint32_t curr_block_idx_;          // (block-level-skipping) The current block index (starts from 0) we're up to during traversal of the inverted list.
  bool block_skipping_;              // (block-level-skipping) Sets whether we should utilize block skipping in NextGEQ().
  bool use_positions_;               // Determines whether position information will be decoded and made available.
  int num_chunks_last_block_left_;   // The number of chunks left to traverse from the last block (only updated when in the last block).
  ChunkDecoder curr_chunk_decoder_;  // The current chunk being processed in this inverted list.
  // This is algorithm dependent, but could potentially be used often.
  float score_threshold_;            // The maximum scoring partial score of any docID in this list.
  int term_num_;                     // May be used by the query processor to map this object back to the term it corresponds to.

  // Used about once per chunk (i.e. when we decode the chunk).
  const CodingPolicy& doc_id_decompressor_;     // DocID decoder.
  const CodingPolicy& frequency_decompressor_;  // Frequency decoder.
  const CodingPolicy& position_decompressor_;   // Position decoder.
  int num_docs_last_chunk_;                     // The number of documents contained in the last chunk of this inverted list.
  int num_docs_left_;                           // The number of documents we have left to traverse in this inverted list (it could be negative by the end).
  int num_blocks_;                              // The total number of blocks in this inverted list.
  int num_blocks_left_;                         // The number of blocks left to traverse in this inverted list.
  uint32_t prev_block_last_doc_id_;             // The last docID of the last chunk of the previous block.
                                                // This is used by the IndexReader as necessary in NextGEQ(). It's necessary for the case when a list spans
                                                // several blocks and the docIDs are gap coded, so that when decoding gaps from docIDs appearing in subsequent
                                                // blocks (subsequent from the first block), we need to know from what offset
                                                // (the last docID of the previous block) those docIDs start at.
  // For simultaneous traversal of the external index (if any).
  ExternalIndexReader::ExternalIndexPointer external_index_pointer_;
  const ExternalIndexReader* external_index_reader_;

  // Used about once per block (i.e. when we advance the block).
  CacheManager& cache_manager_;                    // Used to retrieve blocks from the inverted list.
  const CodingPolicy& block_header_decompressor_;  // Block header decoder.
  const int kReadAheadBlocks;                      // The number of blocks we want to read ahead into the cache.
  const uint32_t* last_doc_ids_;                   // (block-level-skipping) Pointer to the array of last docIDs of all the blocks in this list.
  uint32_t curr_block_num_;                        // The current block number we're up to during traversal of the inverted list.
  uint32_t last_queued_block_num_;                 // The last block number that was not yet queued for transfer.
  uint64_t cached_bytes_read_;                     // Keeps track of the number of bytes read from the cache for this list.
  uint64_t disk_bytes_read_;                       // Keeps track of the number of bytes read from the disk for this list.
  uint32_t num_blocks_skipped_;                    // Keeps track of the number of blocks we were able to skip (when using in-memory block index).
};

/**************************************************************************************************************************************************************
 * LexiconData
 *
 * Container for entries in the lexicon.
 *
 * TODO: We'll probably do better here memory-wise just using a c-string, because a NULL terminated string is just one extra byte while using an integer to
 *       store the length is four extra bytes.
 * TODO: If we know the index is single layered, we don't need to store a few of the fields, they'd be just taking up memory.
 **************************************************************************************************************************************************************/
class LexiconData {
public:
  LexiconData(const char* term, int term_len);
  ~LexiconData();

  void InitLayers();
  void InitLayers(int num_layers, const int* num_docs, const int* num_chunks, const int* num_chunks_last_block, const int* num_blocks,
                  const int* block_numbers, const int* chunk_numbers, const float* score_thresholds, const uint32_t* external_index_offsets);

  const char* term() const {
    return term_;
  }

  int term_len() const {
    return term_len_;
  }

  int num_layers() const {
    return num_layers_;
  }

  int layer_num_docs(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.num_docs;
    } else {
      return additional_layers_[layer_num - 1].num_docs;
    }
  }

  int layer_num_chunks(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.num_chunks;
    } else {
      return additional_layers_[layer_num - 1].num_chunks;
    }
  }

  int layer_num_chunks_last_block(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.num_chunks_last_block;
    } else {
      return additional_layers_[layer_num - 1].num_chunks_last_block;
    }
  }

  int layer_num_blocks(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.num_blocks;
    } else {
      return additional_layers_[layer_num - 1].num_blocks;
    }
  }

  int layer_block_number(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.block_number;
    } else {
      return additional_layers_[layer_num - 1].block_number;
    }
  }

  int layer_chunk_number(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.chunk_number;
    } else {
      return additional_layers_[layer_num - 1].chunk_number;
    }
  }

  float layer_score_threshold(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.score_threshold;
    } else {
      return additional_layers_[layer_num - 1].score_threshold;
    }
  }

  uint32_t layer_external_index_offset(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.external_index_offset;
    } else {
      return additional_layers_[layer_num - 1].external_index_offset;
    }
  }

  const uint32_t* layer_last_doc_ids(int layer_num) const {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      return first_layer_.last_doc_ids;
    } else {
      return additional_layers_[layer_num - 1].last_doc_ids;
    }
  }

  void set_last_doc_ids_layer_ptr(uint32_t* last_doc_ids_layer, int layer_num) {
    assert(layer_num < num_layers_);
    if (layer_num == 0) {
      first_layer_.last_doc_ids = last_doc_ids_layer;
    } else {
      additional_layers_[layer_num - 1].last_doc_ids = last_doc_ids_layer;
    }
  }

  LexiconData* next() const {
    return next_;
  }

  void set_next(LexiconData* next) {
    next_ = next;
  }

private:
  int term_len_;  // The length of the 'term_', since it's not NULL terminated.
  char* term_;    // Pointer to the term this lexicon entry holds (it is not NULL terminated!).

  int num_layers_;  // Number of layers this list consists of.

  // Holds the lexicon information for a single layer.
  struct LayerInfo {
    int num_docs;                    // The total number of documents in the inverted list layer for this term.
    int num_chunks;                  // The total number of chunks in the inverted list layer for this term.
    int num_chunks_last_block;       // The number of chunks in the last block in the inverted list layer for this term.
    int num_blocks;                  // The total number of blocks in the inverted list layer for this term.
    int block_number;                // The initial block number of the inverted list layer for this term.
    int chunk_number;                // The initial chunk number in the initial block of the inverted list layer for this term.
    float score_threshold;           // The max BM25 document score in the inverted list layer for this term.
    uint32_t external_index_offset;  // The offset into the external index of the inverted list layer for this term.
    uint32_t* last_doc_ids;          // A pointer to an array of docIDs. The index into the array corresponds to the block number within the inverted list layer
                                     // and the docID is the last docID in the block for this inverted list.
  };

  LayerInfo first_layer_;            // Every list has at least one layer (and most will have only one). Any additional layers will be allocated dynamically.
  LayerInfo* additional_layers_;     // Dynamically allocated for any additional layers.

  LexiconData* next_;                // Pointer to the next lexicon entry.
};

/**************************************************************************************************************************************************************
 * Lexicon
 *
 * TODO: Would be wise to setup a large memory pool (we can even count during lexicon construction how many bytes we'll need). Then the LexiconData class
 *       can just take pointers to data in this memory pool without copying it (or just 1 pointer and decode it on the fly). This means variable sized data
 *       could be stored more compactly, without memory fragmentation. Best of all, we can adapt our decoding technique based on what data we stored in this
 *       memory pool; then we won't have to, say, store layered information if the index is only single layered. This would only be necessary for when we read
 *       the whole lexicon into main memory (not when merging).
 **************************************************************************************************************************************************************/
class Lexicon {
public:
  Lexicon(int hash_table_size, const char* lexicon_filename, bool random_access);
  ~Lexicon();

  void Open(const char* lexicon_filename, bool random_access);
  void Close();

  LexiconData* GetEntry(const char* term, int term_len);

  // Should be used only when index is in 'kMerge' mode.
  // Returns the next lexicon entry lexicographically, NULL if no more.
  LexiconData* GetNextEntry();

  MoveToFrontHashTable<LexiconData>* lexicon() const {
    return lexicon_;
  }

private:
  // All the pointer are pointing to our local buffer. We will copy them to the LexiconData before invalidating the buffer.
  struct LexiconEntry {
    char* term;
    int term_len;

    int num_layers;

    int* num_docs;
    int* num_chunks;
    int* num_chunks_last_block;
    int* num_blocks;
    int* block_numbers;
    int* chunk_numbers;
    float* score_thresholds;
    uint32_t* external_index_offsets;
  };

  void GetNext(LexiconEntry* lexicon_entry);

  MoveToFrontHashTable<LexiconData>* lexicon_;  // The pointer to the hash table for randomly querying the lexicon.
  int kLexiconBufferSize;                       // Size of buffer used for reading parts of the lexicon.
  char* lexicon_buffer_;                        // Pointer to the current portion of the lexicon we're buffering.
  char* lexicon_buffer_ptr_;                    // Current position in the lexicon buffer.
  int lexicon_fd_;                              // File descriptor for the lexicon.
  off_t lexicon_file_size_;                     // The size of the on disk lexicon file.
  off_t num_bytes_read_;                        // Number of bytes of lexicon read so far.
};

/**************************************************************************************************************************************************************
 * IndexReader
 *
 * Provides access to the various structures that comprise an index such as the inverted lists, lexicon, document map, and meta file.
 **************************************************************************************************************************************************************/
class IndexReader {
public:
  enum Purpose {
    kRandomQuery, kMerge
  };

  IndexReader(Purpose purpose, CacheManager& cache_manager, const char* lexicon_filename, const char* doc_map_basic_filename,
              const char* doc_map_extended_filename, const char* meta_info_filename, bool use_positions,
              const ExternalIndexReader* external_index_reader = NULL);

  ListData* OpenList(const LexiconData& lex_data, int layer_num, bool single_term_query = false);
  ListData* OpenList(const LexiconData& lex_data, int layer_num, bool single_term_query, int term_num);
  void CloseList(ListData* list_data);

  Lexicon& lexicon() {
    return lexicon_;
  }

  void ResetStats() {
    total_cached_bytes_read_ = 0;
    total_disk_bytes_read_ = 0;
    total_num_lists_accessed_ = 0;
  }

  const DocumentMapReader& document_map() const {
    return document_map_;
  }

  const IndexConfiguration& meta_info() const {
    return meta_info_;
  }

  uint64_t total_index_bytes() const {
    return cache_manager_.total_index_blocks() * CacheManager::kBlockSize;
  }

  bool includes_contexts() const {
    return includes_contexts_;
  }

  bool includes_positions() const {
    return includes_positions_;
  }

  bool block_skipping_enabled() const {
    return block_skipping_enabled_;
  }

  void set_block_skipping_enabled(bool block_skipping_enabled) {
    block_skipping_enabled_ = block_skipping_enabled;
  }

  const CodingPolicy& doc_id_decompressor() const {
    return doc_id_decompressor_;
  }

  const CodingPolicy& frequency_decompressor() const {
    return frequency_decompressor_;
  }

  const CodingPolicy& position_decompressor() const {
    return position_decompressor_;
  }

  const CodingPolicy& block_header_decompressor() const {
    return block_header_decompressor_;
  }

  uint64_t total_cached_bytes_read() const {
    return total_cached_bytes_read_;
  }

  uint64_t total_disk_bytes_read() const {
    return total_disk_bytes_read_;
  }

  uint64_t total_num_lists_accessed() const {
    return total_num_lists_accessed_;
  }

  uint32_t total_num_blocks_skipped() const {
    return total_num_blocks_skipped_;
  }

private:
  Purpose purpose_;                    // Changes index reader behavior based on what we're using it for.
  const char* kLexiconSizeKey;         // The key in the configuration file used to define the lexicon size.
  const long int kLexiconSize;         // The size of the hash table for the lexicon.
  Lexicon lexicon_;                    // The lexicon data structure.
  DocumentMapReader document_map_;     // The document map data structure.
  CacheManager& cache_manager_;        // Manages the block cache.
  IndexConfiguration meta_info_;       // The index meta information.
  bool includes_contexts_;             // True if the index contains context data.
  bool includes_positions_;            // True if the index contains position data.
  bool use_positions_;                 // A hint from an external source that allows us to speed up processing a bit if it doesn't require positions.
  bool block_skipping_enabled_;        // An in-memory block level index has been built that we should use to skip entire blocks.

  const ExternalIndexReader* external_index_reader_;

  // Decompressors for various portions of the index.
  CodingPolicy doc_id_decompressor_;
  CodingPolicy frequency_decompressor_;
  CodingPolicy position_decompressor_;
  CodingPolicy block_header_decompressor_;

  uint64_t total_cached_bytes_read_;   // Keeps track of the number of bytes read from the cache.
  uint64_t total_disk_bytes_read_;     // Keeps track of the number of bytes read from the disk.
  uint64_t total_num_lists_accessed_;  // Keeps track of the total number of inverted lists that were accessed (updated at the time that the list is closed).
  uint32_t total_num_blocks_skipped_;  // Keeps track of the total number of blocks that were skipped due to the in-memory block index.
};

/**************************************************************************************************************************************************************
 * ListCompare
 *
 * Compares the inverted list lengths, used to sort query terms in order from shortest list to longest.
 **************************************************************************************************************************************************************/
struct ListCompare {
  bool operator()(const ListData* l, const ListData* r) const {
    return l->num_docs() < r->num_docs();
  }
};

#include "index_reader-inl.h"

#endif /* INDEX_READER_H_ */
