/* Copyright 2010 Google Inc. All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/

/* A (forgetful) hash table to the data seen by the compressor, to
   help create backward references to previous data. */

#ifndef BROTLI_ENC_HASH_H_
#define BROTLI_ENC_HASH_H_

#include <string.h>  /* memcmp, memset */

#include "../common/constants.h"
#include "../common/dictionary.h"
#include "../common/platform.h"
#include <brotli/types.h>
#ifdef __VMS
#include "encoder_dict.h"
#else
#include "./encoder_dict.h"
#endif
#ifdef __VMS
#include "fast_log.h"
#else
#include "./fast_log.h"
#endif
#ifdef __VMS
#include "find_match_length.h"
#else
#include "./find_match_length.h"
#endif
#ifdef __VMS
#include "memory.h"
#else
#include "./memory.h"
#endif
#ifdef __VMS
#include "quality.h"
#else
#include "./quality.h"
#endif
#ifdef __VMS
#include "static_dict.h"
#else
#include "./static_dict.h"
#endif

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

typedef struct {
  /* Dynamically allocated area; first member for quickest access. */
  void* extra;

  size_t dict_num_lookups;
  size_t dict_num_matches;

  BrotliHasherParams params;

  /* False if hasher needs to be "prepared" before use. */
  BROTLI_BOOL is_prepared_;
} HasherCommon;

#define score_t size_t

static const uint32_t kCutoffTransformsCount = 10;
/*   0,  12,   27,    23,    42,    63,    56,    48,    59,    64 */
/* 0+0, 4+8, 8+19, 12+11, 16+26, 20+43, 24+32, 28+20, 32+27, 36+28 */
static const uint64_t kCutoffTransforms =
    BROTLI_MAKE_UINT64_T(0x071B520A, 0xDA2D3200);

typedef struct HasherSearchResult {
  size_t len;
  size_t distance;
  score_t score;
  int len_code_delta; /* == len_code - len */
} HasherSearchResult;

/* kHashMul32 multiplier has these properties:
   * The multiplier must be odd. Otherwise we may lose the highest bit.
   * No long streaks of ones or zeros.
   * There is no effort to ensure that it is a prime, the oddity is enough
     for this use.
   * The number has been tuned heuristically against compression benchmarks. */
static const uint32_t kHashMul32 = 0x1E35A7BD;
static const uint64_t kHashMul64 = BROTLI_MAKE_UINT64_T(0x1E35A7BD, 0x1E35A7BD);
static const uint64_t kHashMul64Long =
    BROTLI_MAKE_UINT64_T(0x1FE35A7Bu, 0xD3579BD3u);

static BROTLI_INLINE uint32_t Hash14(const uint8_t* data) {
  uint32_t h = BROTLI_UNALIGNED_LOAD32LE(data) * kHashMul32;
  /* The higher bits contain more mixture from the multiplication,
     so we take our results from there. */
  return h >> (32 - 14);
}

static BROTLI_INLINE void PrepareDistanceCache(
    int* BROTLI_RESTRICT distance_cache, const int num_distances) {
  if (num_distances > 4) {
    int last_distance = distance_cache[0];
    distance_cache[4] = last_distance - 1;
    distance_cache[5] = last_distance + 1;
    distance_cache[6] = last_distance - 2;
    distance_cache[7] = last_distance + 2;
    distance_cache[8] = last_distance - 3;
    distance_cache[9] = last_distance + 3;
    if (num_distances > 10) {
      int next_last_distance = distance_cache[1];
      distance_cache[10] = next_last_distance - 1;
      distance_cache[11] = next_last_distance + 1;
      distance_cache[12] = next_last_distance - 2;
      distance_cache[13] = next_last_distance + 2;
      distance_cache[14] = next_last_distance - 3;
      distance_cache[15] = next_last_distance + 3;
    }
  }
}

#define BROTLI_LITERAL_BYTE_SCORE 135
#define BROTLI_DISTANCE_BIT_PENALTY 30
/* Score must be positive after applying maximal penalty. */
#define BROTLI_SCORE_BASE (BROTLI_DISTANCE_BIT_PENALTY * 8 * sizeof(size_t))

/* Usually, we always choose the longest backward reference. This function
   allows for the exception of that rule.

   If we choose a backward reference that is further away, it will
   usually be coded with more bits. We approximate this by assuming
   log2(distance). If the distance can be expressed in terms of the
   last four distances, we use some heuristic constants to estimate
   the bits cost. For the first up to four literals we use the bit
   cost of the literals from the literal cost model, after that we
   use the average bit cost of the cost model.

   This function is used to sometimes discard a longer backward reference
   when it is not much longer and the bit cost for encoding it is more
   than the saved literals.

   backward_reference_offset MUST be positive. */
static BROTLI_INLINE score_t BackwardReferenceScore(
    size_t copy_length, size_t backward_reference_offset) {
  return BROTLI_SCORE_BASE + BROTLI_LITERAL_BYTE_SCORE * (score_t)copy_length -
      BROTLI_DISTANCE_BIT_PENALTY * Log2FloorNonZero(backward_reference_offset);
}

static BROTLI_INLINE score_t BackwardReferenceScoreUsingLastDistance(
    size_t copy_length) {
  return BROTLI_LITERAL_BYTE_SCORE * (score_t)copy_length +
      BROTLI_SCORE_BASE + 15;
}

static BROTLI_INLINE score_t BackwardReferencePenaltyUsingLastDistance(
    size_t distance_short_code) {
  return (score_t)39 + ((0x1CA10 >> (distance_short_code & 0xE)) & 0xE);
}

static BROTLI_INLINE BROTLI_BOOL TestStaticDictionaryItem(
    const BrotliEncoderDictionary* dictionary, size_t len, size_t word_idx,
    const uint8_t* data, size_t max_length, size_t max_backward,
    size_t max_distance, HasherSearchResult* out) {
  size_t offset;
  size_t matchlen;
  size_t backward;
  score_t score;
  offset = dictionary->words->offsets_by_length[len] + len * word_idx;
  if (len > max_length) {
    return BROTLI_FALSE;
  }

  matchlen =
      FindMatchLengthWithLimit(data, &dictionary->words->data[offset], len);
  if (matchlen + dictionary->cutoffTransformsCount <= len || matchlen == 0) {
    return BROTLI_FALSE;
  }
  {
    size_t cut = len - matchlen;
    size_t transform_id = (cut << 2) +
        (size_t)((dictionary->cutoffTransforms >> (cut * 6)) & 0x3F);
    backward = max_backward + 1 + word_idx +
        (transform_id << dictionary->words->size_bits_by_length[len]);
  }
  if (backward > max_distance) {
    return BROTLI_FALSE;
  }
  score = BackwardReferenceScore(matchlen, backward);
  if (score < out->score) {
    return BROTLI_FALSE;
  }
  out->len = matchlen;
  out->len_code_delta = (int)len - (int)matchlen;
  out->distance = backward;
  out->score = score;
  return BROTLI_TRUE;
}

static BROTLI_INLINE void SearchInStaticDictionary(
    const BrotliEncoderDictionary* dictionary,
    HasherCommon* common, const uint8_t* data, size_t max_length,
    size_t max_backward, size_t max_distance,
    HasherSearchResult* out, BROTLI_BOOL shallow) {
  size_t key;
  size_t i;
  if (common->dict_num_matches < (common->dict_num_lookups >> 7)) {
    return;
  }
  key = Hash14(data) << 1;
  for (i = 0; i < (shallow ? 1u : 2u); ++i, ++key) {
    common->dict_num_lookups++;
    if (dictionary->hash_table_lengths[key] != 0) {
      BROTLI_BOOL item_matches = TestStaticDictionaryItem(
          dictionary, dictionary->hash_table_lengths[key],
          dictionary->hash_table_words[key], data,
          max_length, max_backward, max_distance, out);
      if (item_matches) {
        common->dict_num_matches++;
      }
    }
  }
}

typedef struct BackwardMatch {
  uint32_t distance;
  uint32_t length_and_code;
} BackwardMatch;

static BROTLI_INLINE void InitBackwardMatch(BackwardMatch* self,
    size_t dist, size_t len) {
  self->distance = (uint32_t)dist;
  self->length_and_code = (uint32_t)(len << 5);
}

static BROTLI_INLINE void InitDictionaryBackwardMatch(BackwardMatch* self,
    size_t dist, size_t len, size_t len_code) {
  self->distance = (uint32_t)dist;
  self->length_and_code =
      (uint32_t)((len << 5) | (len == len_code ? 0 : len_code));
}

static BROTLI_INLINE size_t BackwardMatchLength(const BackwardMatch* self) {
  return self->length_and_code >> 5;
}

static BROTLI_INLINE size_t BackwardMatchLengthCode(const BackwardMatch* self) {
  size_t code = self->length_and_code & 31;
  return code ? code : BackwardMatchLength(self);
}

#define EXPAND_CAT(a, b) CAT(a, b)
#define CAT(a, b) a ## b
#define FN(X) EXPAND_CAT(X, HASHER())

#define HASHER() H10
#define BUCKET_BITS 17
#define MAX_TREE_SEARCH_DEPTH 64
#define MAX_TREE_COMP_LENGTH 128
#ifdef __VMS
#include "hash_to_binary_tree_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_to_binary_tree_inc.h"  /* NOLINT(build/include) */
#endif
#undef MAX_TREE_SEARCH_DEPTH
#undef MAX_TREE_COMP_LENGTH
#undef BUCKET_BITS
#undef HASHER
/* MAX_NUM_MATCHES == 64 + MAX_TREE_SEARCH_DEPTH */
#define MAX_NUM_MATCHES_H10 128

/* For BUCKET_SWEEP_BITS == 0, enabling the dictionary lookup makes compression
   a little faster (0.5% - 1%) and it compresses 0.15% better on small text
   and HTML inputs. */

#define HASHER() H2
#define BUCKET_BITS 16
#define BUCKET_SWEEP_BITS 0
#define HASH_LEN 5
#define USE_DICTIONARY 1
#ifdef __VMS
#include "hash_longest_match_quickly_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_longest_match_quickly_inc.h"  /* NOLINT(build/include) */
#endif
#undef BUCKET_SWEEP_BITS
#undef USE_DICTIONARY
#undef HASHER

#define HASHER() H3
#define BUCKET_SWEEP_BITS 1
#define USE_DICTIONARY 0
#ifdef __VMS
#include "hash_longest_match_quickly_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_longest_match_quickly_inc.h"  /* NOLINT(build/include) */
#endif
#undef USE_DICTIONARY
#undef BUCKET_SWEEP_BITS
#undef BUCKET_BITS
#undef HASHER

#define HASHER() H4
#define BUCKET_BITS 17
#define BUCKET_SWEEP_BITS 2
#define USE_DICTIONARY 1
#ifdef __VMS
#include "hash_longest_match_quickly_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_longest_match_quickly_inc.h"  /* NOLINT(build/include) */
#endif
#undef USE_DICTIONARY
#undef HASH_LEN
#undef BUCKET_SWEEP_BITS
#undef BUCKET_BITS
#undef HASHER

#define HASHER() H5
#ifdef __VMS
#include "hash_longest_match_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_longest_match_inc.h"  /* NOLINT(build/include) */
#endif
#undef HASHER

#define HASHER() H6
#ifdef __VMS
#include "hash_longest_match64_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_longest_match64_inc.h"  /* NOLINT(build/include) */
#endif
#undef HASHER

#define BUCKET_BITS 15

#define NUM_LAST_DISTANCES_TO_CHECK 4
#define NUM_BANKS 1
#define BANK_BITS 16
#define HASHER() H40
#ifdef __VMS
#include "hash_forgetful_chain_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_forgetful_chain_inc.h"  /* NOLINT(build/include) */
#endif
#undef HASHER
#undef NUM_LAST_DISTANCES_TO_CHECK

#define NUM_LAST_DISTANCES_TO_CHECK 10
#define HASHER() H41
#ifdef __VMS
#include "hash_forgetful_chain_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_forgetful_chain_inc.h"  /* NOLINT(build/include) */
#endif
#undef HASHER
#undef NUM_LAST_DISTANCES_TO_CHECK
#undef NUM_BANKS
#undef BANK_BITS

#define NUM_LAST_DISTANCES_TO_CHECK 16
#define NUM_BANKS 512
#define BANK_BITS 9
#define HASHER() H42
#ifdef __VMS
#include "hash_forgetful_chain_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_forgetful_chain_inc.h"  /* NOLINT(build/include) */
#endif
#undef HASHER
#undef NUM_LAST_DISTANCES_TO_CHECK
#undef NUM_BANKS
#undef BANK_BITS

#undef BUCKET_BITS

#define HASHER() H54
#define BUCKET_BITS 20
#define BUCKET_SWEEP_BITS 2
#define HASH_LEN 7
#define USE_DICTIONARY 0
#ifdef __VMS
#include "hash_longest_match_quickly_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_longest_match_quickly_inc.h"  /* NOLINT(build/include) */
#endif
#undef USE_DICTIONARY
#undef HASH_LEN
#undef BUCKET_SWEEP_BITS
#undef BUCKET_BITS
#undef HASHER

/* fast large window hashers */

#define HASHER() HROLLING_FAST
#define CHUNKLEN 32
#define JUMP 4
#define NUMBUCKETS 16777216
#define MASK ((NUMBUCKETS * 64) - 1)
#ifdef __VMS
#include "hash_rolling_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_rolling_inc.h"  /* NOLINT(build/include) */
#endif
#undef JUMP
#undef HASHER


#define HASHER() HROLLING
#define JUMP 1
#ifdef __VMS
#include "hash_rolling_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_rolling_inc.h"  /* NOLINT(build/include) */
#endif
#undef MASK
#undef NUMBUCKETS
#undef JUMP
#undef CHUNKLEN
#undef HASHER

#define HASHER() H35
#define HASHER_A H3
#define HASHER_B HROLLING_FAST
#ifdef __VMS
#include "hash_composite_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_composite_inc.h"  /* NOLINT(build/include) */
#endif
#undef HASHER_A
#undef HASHER_B
#undef HASHER

#define HASHER() H55
#define HASHER_A H54
#define HASHER_B HROLLING_FAST
#ifdef __VMS
#include "hash_composite_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_composite_inc.h"  /* NOLINT(build/include) */
#endif
#undef HASHER_A
#undef HASHER_B
#undef HASHER

#define HASHER() H65
#define HASHER_A H6
#define HASHER_B HROLLING
#ifdef __VMS
#include "hash_composite_inc.h"  /* NOLINT(build/include) */
#else
#include "./hash_composite_inc.h"  /* NOLINT(build/include) */
#endif
#undef HASHER_A
#undef HASHER_B
#undef HASHER

#undef FN
#undef CAT
#undef EXPAND_CAT

#define FOR_SIMPLE_HASHERS(H) H(2) H(3) H(4) H(5) H(6) H(40) H(41) H(42) H(54)
#define FOR_COMPOSITE_HASHERS(H) H(35) H(55) H(65)
#define FOR_GENERIC_HASHERS(H) FOR_SIMPLE_HASHERS(H) FOR_COMPOSITE_HASHERS(H)
#define FOR_ALL_HASHERS(H) FOR_GENERIC_HASHERS(H) H(10)

typedef struct {
  HasherCommon common;

  union {
#define MEMBER_(N) \
    H ## N _H ## N;
    FOR_ALL_HASHERS(MEMBER_)
#undef MEMBER_
  } privat;
} Hasher;

/* MUST be invoked before any other method. */
static BROTLI_INLINE void HasherInit(Hasher* hasher) {
  hasher->common.extra = NULL;
}

static BROTLI_INLINE void DestroyHasher(MemoryManager* m, Hasher* hasher) {
  if (hasher->common.extra == NULL) return;
  BROTLI_FREE(m, hasher->common.extra);
}

static BROTLI_INLINE void HasherReset(Hasher* hasher) {
  hasher->common.is_prepared_ = BROTLI_FALSE;
}

static BROTLI_INLINE size_t HasherSize(const BrotliEncoderParams* params,
    BROTLI_BOOL one_shot, const size_t input_size) {
  switch (params->hasher.type) {
#define SIZE_(N)                                                      \
    case N:                                                           \
      return HashMemAllocInBytesH ## N(params, one_shot, input_size);
    FOR_ALL_HASHERS(SIZE_)
#undef SIZE_
    default:
      break;
  }
  return 0;  /* Default case. */
}

static BROTLI_INLINE void HasherSetup(MemoryManager* m, Hasher* hasher,
    BrotliEncoderParams* params, const uint8_t* data, size_t position,
    size_t input_size, BROTLI_BOOL is_last) {
  BROTLI_BOOL one_shot = (position == 0 && is_last);
  if (hasher->common.extra == NULL) {
    size_t alloc_size;
    ChooseHasher(params, &params->hasher);
    alloc_size = HasherSize(params, one_shot, input_size);
    hasher->common.extra = BROTLI_ALLOC(m, uint8_t, alloc_size);
    if (BROTLI_IS_OOM(m) || BROTLI_IS_NULL(hasher->common.extra)) return;
    hasher->common.params = params->hasher;
    switch (hasher->common.params.type) {
#define INITIALIZE_(N)                        \
      case N:                                 \
        InitializeH ## N(&hasher->common,     \
            &hasher->privat._H ## N, params); \
        break;
      FOR_ALL_HASHERS(INITIALIZE_);
#undef INITIALIZE_
      default:
        break;
    }
    HasherReset(hasher);
  }

  if (!hasher->common.is_prepared_) {
    switch (hasher->common.params.type) {
#define PREPARE_(N)                      \
      case N:                            \
        PrepareH ## N(                   \
            &hasher->privat._H ## N,     \
            one_shot, input_size, data); \
        break;
      FOR_ALL_HASHERS(PREPARE_)
#undef PREPARE_
      default: break;
    }
    if (position == 0) {
      hasher->common.dict_num_lookups = 0;
      hasher->common.dict_num_matches = 0;
    }
    hasher->common.is_prepared_ = BROTLI_TRUE;
  }
}

static BROTLI_INLINE void InitOrStitchToPreviousBlock(
    MemoryManager* m, Hasher* hasher, const uint8_t* data, size_t mask,
    BrotliEncoderParams* params, size_t position, size_t input_size,
    BROTLI_BOOL is_last) {
  HasherSetup(m, hasher, params, data, position, input_size, is_last);
  if (BROTLI_IS_OOM(m)) return;
  switch (hasher->common.params.type) {
#define INIT_(N)                             \
    case N:                                  \
      StitchToPreviousBlockH ## N(           \
          &hasher->privat._H ## N,           \
          input_size, position, data, mask); \
    break;
    FOR_ALL_HASHERS(INIT_)
#undef INIT_
    default: break;
  }
}

#if defined(__cplusplus) || defined(c_plusplus)
}  /* extern "C" */
#endif

#endif  /* BROTLI_ENC_HASH_H_ */
