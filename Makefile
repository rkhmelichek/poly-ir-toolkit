CXXFLAGS =	-O3 -g -Wall -msse3 -Wstrict-aliasing
			#-DIRTK_DEBUG      : defines IRTK_DEBUG to turn on internal debugging code.
			#-DNDEBUG          : defines NDEBUG to turn off assertions.
			#-pg               : enables code profiling.
			#-Wvla             : warns about variable length arrays.
			#-Winline          : warns about functions declared inline but were not actually inlined.
			#-msse3            : enables support for SSE3 instructions.
			#-mtune=core2      : tunes code generation for a specific microarchitecture.
			#-fstrict-aliasing : enables the compiler to apply certain optimizations (this requires that no pointers of different types point to the same location in memory, with a few exceptions).
			#-Wstrict-aliasing : warns about code that doesn't follow strict aliasing (does not necessarily warn about all cases).

# 'make DEBUG=YES' to enable the -DIRTK_DEBUG compilation flag.
ifeq ($(DEBUG), YES)
	CXXFLAGS += -DIRTK_DEBUG
endif

# 'make DEBUG=NO' to enable the -DNDEBUG compilation flag.
ifeq ($(DEBUG), NO)
	CXXFLAGS += -DNDEBUG
endif

OBJS =		src/cache_manager.o \
			src/coding_policy.o \
			src/coding_policy_helper.o \
			src/configuration.o \
			src/document_collection.o \
			src/document_map.o \
			src/external_index.o \
			src/globals.o \
			src/index_build.o \
			src/index_cat.o \
			src/index_configuration.o \
			src/index_diff.o \
			src/index_layerify.o \
			src/index_merge.o \
			src/index_reader.o \
			src/index_remapper.o \
			src/index_util.o \
			src/integer_hash_table.o \
			src/ir_toolkit.o \
			src/key_value_store.o \
			src/logger.o \
			src/parser.o \
 			src/parser_callback.o \
			src/posting_collection.o \
			src/query_processor.o \
			src/test_compression.o \
			src/timer.o \
			src/term_hash_table.o \
			src/uncompress_file.o \
			src/compression_toolkit/coding_factory.o \
			src/compression_toolkit/null_coding.o \
			src/compression_toolkit/pfor_coding.o \
			src/compression_toolkit/rice_coding.o \
			src/compression_toolkit/rice_coding2.o \
			src/compression_toolkit/s9_coding.o \
			src/compression_toolkit/s16_coding.o \
			src/compression_toolkit/vbyte_coding.o \
			src/compression_toolkit/unpack.o

LIBS =		-lpthread -lrt -lz
			#-lpthread            : pthreads support.
			#-lrt                 : necessary for POSIX Asynchronous I/O support (AIO).
			#-lz                  : zlib support.
			#-static-libgcc -lc_p : profiling support for C library calls.
			#-lefence             : electric fence memory debugger (only used for debugging).

TARGET =	irtk

$(TARGET):	$(OBJS)
	$(CXX) -o $(TARGET) $(OBJS) $(LIBS)
	#-pg : enables code profiling.

all:	$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

clear:
	rm -f document_collections_doc_id_ranges index*.idx* index*.lex* index*.meta* index*.dmap* index*.ext*
