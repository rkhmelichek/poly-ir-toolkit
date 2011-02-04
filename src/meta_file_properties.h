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
// Defines standard properties that are used by index meta file readers and writers.
//==============================================================================================================================================================

#ifndef META_FILE_PROPERTIES_H_
#define META_FILE_PROPERTIES_H_

namespace meta_properties {

// Whether the index has been remapped (that is, the docIDs have been reordered based upon some mapping).
// If the index is remapped, the document map automatically loads the mapping file (currently, must be named 'url_sorted_doc_id_mapping'),
// which is used to get the proper document lengths and URLs for the remapped docIDs.
static const char kRemappedIndex[] = "remapped_index";

// Whether the index is layered.
static const char kLayeredIndex[] = "layered_index";

// The (max) number of layers that the index was built with (only for layered indices).
static const char kNumLayers[] = "num_layers";

// Whether the index layers are overlapping (only for layered indices).
static const char kOverlappingLayers[] = "overlapping_layers";

// Whether the index was indexed with position data.
static const char kIncludesPositions[] = "includes_positions";

// Whether the index was indexed with context data.
static const char kIncludesContexts[] = "includes_contexts";

// The coding policy with which the docIDs are compressed.
static const char kIndexDocIdCoding[] = "index_doc_id_coding";

// The coding policy with which the frequencies are compressed.
static const char kIndexFrequencyCoding[] = "index_frequency_coding";

// The coding policy with which the positions are compressed, if they were included in the index.
static const char kIndexPositionCoding[] = "index_position_coding";

// The coding policy with which the block headers are compressed.
static const char kIndexBlockHeaderCoding[] = "index_block_header_coding";

// The total number of chunks in this index.
static const char kTotalNumChunks[] = "total_num_chunks";

// The total number of per term blocks in this index (as if each term had it's own block, not shared with other terms).
static const char kTotalNumPerTermBlocks[] = "total_num_per_term_blocks";

// The sum of all document lengths in the index (length is based on the position of the last posting relating to a particular docID).
static const char kTotalDocumentLengths[] = "total_document_lengths";

// The total number of documents input to the indexer.
static const char kTotalNumDocs[] = "total_num_docs";

// The number of unique docIDs in the index. This may be different from 'kTotalNumDocs' when some documents don't produce any postings inserted into the index.
static const char kTotalUniqueNumDocs[] = "total_unique_num_docs";

// The first (lowest) docID that is contained in this index.
static const char kFirstDocId[] = "first_doc_id";

// The last (highest) docID that is contained in this index.
static const char kLastDocId[] = "last_doc_id";

// The number of unique terms contained in this index. This is the number of terms in the lexicon.
static const char kNumUniqueTerms[] = "num_unique_terms";

// The number of postings provided as input to the indexer (it may not index all of them; limits defined in 'index_layout_parameters.h').
static const char kDocumentPostingCount[] = "document_posting_count";

// The number of postings actually indexed.
static const char kIndexPostingCount[] = "index_posting_count";

// The total number of bytes used for the block headers in this index.
static const char kTotalHeaderBytes[] = "total_header_bytes";

// The total number of bytes used for the docIDs in this index.
static const char kTotalDocIdBytes[] = "total_doc_id_bytes";

// The total number of bytes used for the frequencies in this index.
static const char kTotalFrequencyBytes[] = "total_frequency_bytes";

// The total number of bytes used for the positions in this index.
static const char kTotalPositionBytes[] = "total_position_bytes";

// The total number of bytes used to pad the blocks to fill them to exactly block size.
static const char kTotalWastedBytes[] = "total_wasted_bytes";

} // namespace meta_properties

#endif /* META_FILE_PROPERTIES_H_ */
