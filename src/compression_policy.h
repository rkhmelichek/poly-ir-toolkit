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
// TODO: Ideas:
// For the methods that require padding until blocksize elements:
// Need to choose initial compression alg (that requires padding) + another compression alg for the special case where we don't pad.
//
//
//
//==============================================================================================================================================================

#ifndef COMPRESSION_POLICY_H_
#define COMPRESSION_POLICY_H_

#include "index_layout_parameters.h"

// NOTES:
// GetCompressionPolicy
// let it parse the expression (or just have method for parsing and another to just enter params)
// it will init the proper policy --> use the base coding class for the compressor and leftover compressor...

// ENCODING::
// pass pointer to compressed data and decompressed space (assert there is enough space) ---> also have a function that will return the amount of decompressed space needed

// DECODING::
//

// Defines chunk-wise compression policies.
class CompressionPolicy {
public:
  CompressionPolicy* GetCompressionPolicy(bool compression, int policy);

};

class PForCompressionPolicy : CompressionPolicy {

};

class S16CompressionPolicy : CompressionPolicy {

};

#endif /* COMPRESSION_POLICY_H_ */
