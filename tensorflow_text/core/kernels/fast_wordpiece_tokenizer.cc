// Copyright 2023 TF.Text Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tensorflow_text/core/kernels/fast_wordpiece_tokenizer.h"

#include <memory>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "icu4c/source/common/unicode/uchar.h"
#include "icu4c/source/common/unicode/utf8.h"
#include "tensorflow/lite/kernels/shim/status_macros.h"
#include "tensorflow_text/core/kernels/fast_wordpiece_tokenizer_utils.h"

namespace tensorflow {
namespace text {
namespace {

template <bool kGetPieces>
int GetCurrentOutputSize(std::vector<std::string>* output_pieces,
                         std::vector<int>* output_ids) {
  if constexpr (kGetPieces) {
    return output_pieces->size();
  } else {
    return output_ids->size();
  }
}

}  // namespace

/*static*/ absl::StatusOr<FastWordpieceTokenizer>
FastWordpieceTokenizer::Create(const void* config_flatbuffer) {
  FastWordpieceTokenizer tokenizer;
  // `GetFastWordpieceTokenizerConfig()` is autogenerated by flatbuffer.
  tokenizer.config_ = GetFastWordpieceTokenizerConfig(config_flatbuffer);
  auto trie_or = trie_utils::DartsCloneTrieWrapper::Create(
      tokenizer.config_->trie_array()->data());
  if (!trie_or.ok()) {
    return absl::InvalidArgumentError(
        "Failed to create DartsCloneTrieWrapper from "
        "FastWordpieceTokenizerConfig.trie_array.");
  }
  tokenizer.trie_ =
      std::make_unique<trie_utils::DartsCloneTrieWrapper>(*std::move(trie_or));
  return std::move(tokenizer);
}

void FastWordpieceTokenizer::Tokenize(absl::string_view input,
                                      std::vector<std::string>* output_pieces,
                                      std::vector<int>* output_ids,
                                      std::vector<int>* output_start_offsets,
                                      std::vector<int>* output_end_offsets,
                                      int input_word_offset_in_text) const {
  if (config_->end_to_end()) {
    TokenizeTextImpl</*kGetPieces=*/true, /*kGetIds=*/true,
                     /*kGetOffsets=*/true>(input, output_pieces, output_ids,
                                           output_start_offsets,
                                           output_end_offsets);
  } else {
    TokenizeSingleWordImpl</*kGetPieces=*/true, /*kGetIds=*/true,
                           /*kGetOffsets=*/true>(
        input, input_word_offset_in_text, output_pieces, output_ids,
        output_start_offsets, output_end_offsets);
  }
}

void FastWordpieceTokenizer::Tokenize(absl::string_view input,
                                      std::vector<int>* output_ids,
                                      std::vector<int>* output_start_offsets,
                                      std::vector<int>* output_end_offsets,
                                      int input_word_offset_in_text) const {
  if (config_->end_to_end()) {
    TokenizeTextImpl</*kGetPieces=*/false, /*kGetIds=*/true,
                     /*kGetOffsets=*/true>(input, /*output_pieces=*/nullptr,
                                           output_ids, output_start_offsets,
                                           output_end_offsets);
  } else {
    TokenizeSingleWordImpl</*kGetPieces=*/false, /*kGetIds=*/true,
                           /*kGetOffsets=*/true>(
        input, input_word_offset_in_text, /*output_pieces=*/nullptr, output_ids,
        output_start_offsets, output_end_offsets);
  }
}

void FastWordpieceTokenizer::Tokenize(absl::string_view input,
                                      std::vector<int>* output_ids,
                                      int input_word_offset_in_text) const {
  if (config_->end_to_end()) {
    TokenizeTextImpl</*kGetPieces=*/false, /*kGetIds=*/true,
                     /*kGetOffsets=*/false>(input, /*output_pieces=*/nullptr,
                                            output_ids,
                                            /*output_start_offsets=*/nullptr,
                                            /*output_end_offsets=*/nullptr);
  } else {
    TokenizeSingleWordImpl</*kGetPieces=*/false, /*kGetIds=*/true,
                           /*kGetOffsets=*/false>(
        input, input_word_offset_in_text, /*output_pieces=*/nullptr, output_ids,
        /*output_start_offsets=*/nullptr,
        /*output_end_offsets=*/nullptr);
  }
}

absl::StatusOr<std::vector<std::string>>
FastWordpieceTokenizer::DetokenizeToTokens(
    const absl::Span<const int> input) const {
  std::vector<std::string> subwords;
  std::vector<std::string> output_tokens;
  if (!config_->support_detokenization()) {
    return absl::FailedPreconditionError(
        "Detokenize function is only enabled when support_detokenization is "
        "true in the config flatbuffer. Please rebuild the model flatbuffer "
        "by setting support_detokenization=true.");
  }
  for (int id : input) {
    auto vocab = config_->vocab_array()->Get(id);
    auto is_suffix = config_->vocab_is_suffix_array()->Get(id);
    if (!subwords.empty() && !is_suffix) {
      // When current subword is not a suffix token, it marks the start of a new
      // word. We concatenate the subwords that compose the previous word and
      // add it to the return list.
      output_tokens.emplace_back(absl::StrJoin(subwords, ""));
      subwords.clear();
    }
    // Special case: when a suffix token e.g. "##a" appears at the start of the
    // input ids, we preserve the suffix_indicator.
    if (subwords.empty() && is_suffix) {
      subwords.emplace_back(config_->suffix_indicator()->string_view());
    }
    subwords.emplace_back(vocab->string_view());
  }
  if (!subwords.empty()) {
    output_tokens.emplace_back(absl::StrJoin(subwords, ""));
  }
  return output_tokens;
}

absl::StatusOr<std::string> FastWordpieceTokenizer::Detokenize(
    const absl::Span<const int> input) const {
  SH_ASSIGN_OR_RETURN(std::vector<std::string> output_tokens,
                      DetokenizeToTokens(input));
  return absl::StrJoin(output_tokens, " ");
}

int FastWordpieceTokenizer::SkipTheRemainingOfWordAndTrailingWhiteSpaces(
    absl::string_view input, int& cur_pos) const {
  const int input_size = input.size();
  UChar32 cur_unicode_char;
  int next_pos;
  int end_of_word = cur_pos;
  while (cur_pos < input_size) {
    next_pos = cur_pos;
    U8_NEXT(input, next_pos, input_size, cur_unicode_char);
    if (u_isUWhiteSpace(cur_unicode_char)) {
      cur_pos = next_pos;  // Skip the whitespace as well.
      // Break and return since we've met a word boundary.
      break;
    }
    if (fast_wordpiece_tokenizer_utils::IsPunctuationOrChineseChar(
            cur_unicode_char)) {
      // Break and return since we've met a word boundary. We do not skip the
      // punctuation character: that character may be a token by itself.
      break;
    }
    end_of_word = next_pos;  // Mark the exclusive end.
    cur_pos = next_pos;      // Skip the character.
  }
  return end_of_word;
}

template <bool kGetPieces, bool kGetIds, bool kGetOffsets>
void FastWordpieceTokenizer::TokenizeTextImpl(
    absl::string_view input_text, std::vector<std::string>* output_pieces,
    std::vector<int>* output_ids, std::vector<int>* output_start_offsets,
    std::vector<int>* output_end_offsets) const {
  static_assert(kGetPieces || kGetIds,
                "At least one of `kGetPieces` and `kGetIds` should be true.");
  if (input_text.empty()) {
    return;
  }
  const int input_size = input_text.size();
  int next_pos = 0;
  int cur_pos = 0;
  int original_num_tokens =
      GetCurrentOutputSize<kGetPieces>(output_pieces, output_ids);
  UChar32 prev_unicode_char;
  UChar32 cur_unicode_char = 0;
  while (cur_pos < input_size) {
    int cur_offset_in_input_word = 0;
    // Tokenize the word starting at the current position.
    auto cur_node = trie_->CreateTraversalCursorPointToRoot();
    int word_byte_length_so_far = 0;
    int input_word_offset_in_text = cur_pos;
    absl::string_view input_substr = input_text.substr(cur_pos);
    // The trie matching loop below tokenizes and recognizes word pieces until
    //  1. it steps over the input boundary, or
    //  2. the length of the current word reaches 'max_bytes_per_token', or
    //  3. it sees a whitespace / punctuation / unknown character.
    while (cur_pos < input_size) {
      prev_unicode_char = cur_unicode_char;
      next_pos = cur_pos;
      U8_NEXT(input_text, next_pos, input_text.length(), cur_unicode_char);

      if (word_byte_length_so_far + next_pos - cur_pos >
          config_->max_bytes_per_token())
        break;
      // Try matching one Unicode character from here.
      while (!trie_->TryTraverseSeveralSteps(
          cur_node, input_text.substr(cur_pos, next_pos - cur_pos))) {
        // Trie cannot consume the whole Unicode character. We need to pop one
        // or more longest-matching tokens off the beginning of the string
        // represented by the current node. We then transit to the node pointed
        // by the failure link, which represents the remaining suffix string
        // after popping those matching prefix tokens.
        //
        // For example, if the current node is "abcdef", and we need to pop
        // "ab", and "##cd" off the beginning, the failure link points to the
        // node that represents "##ef".
        if (!TryFollowFailureLinkAndCollectTokens<kGetPieces, kGetIds,
                                                  kGetOffsets>(
                input_substr, input_word_offset_in_text,
                cur_offset_in_input_word, cur_node, output_pieces, output_ids,
                output_start_offsets, output_end_offsets)) {
          goto outside_trie_match_loop;
        }
      }
      // Trie consumed the whole Unicode char and was able to traverse to a
      // new node. We move forward the cursor to match the next character.
      word_byte_length_so_far += next_pos - cur_pos;
      cur_pos = next_pos;
    }
  outside_trie_match_loop:
    if (cur_pos >= input_size) {
      // Collect the remaining tokens stored on a path on the trie.
      HandleTheRemainingStringOnTriePath<kGetPieces, kGetIds, kGetOffsets>(
          input_substr, input_word_offset_in_text, cur_node,
          original_num_tokens, cur_offset_in_input_word, output_pieces,
          output_ids, output_start_offsets, output_end_offsets);
      // Break as we've finished all characters.
      break;
    }
    bool is_white_space = u_isUWhiteSpace(cur_unicode_char);
    if (is_white_space ||
        fast_wordpiece_tokenizer_utils::IsPunctuationOrChineseChar(
            cur_unicode_char) ||
        (cur_pos && fast_wordpiece_tokenizer_utils::IsPunctuationOrChineseChar(
                        prev_unicode_char))) {
      // If the current Unicode character is a valid word boundary, collect the
      // remaining tokens stored on a path on the trie.
      HandleTheRemainingStringOnTriePath<kGetPieces, kGetIds, kGetOffsets>(
          absl::string_view(input_substr.data(),
                            cur_pos - input_word_offset_in_text),
          input_word_offset_in_text, cur_node, original_num_tokens,
          cur_offset_in_input_word, output_pieces, output_ids,
          output_start_offsets, output_end_offsets);
      // Skip the whitespace.
      if (is_white_space) cur_pos = next_pos;
      // Continue in the outer while loop to process the remaining input.
      continue;
    }

    // Note that even with the following line removed, the code is still correct
    // (i.e., Mutants is right). We keep this line for efficiency reasons: We
    // have tested the current char, and it is not a whitespace or punctuation
    // char. Hence it's safe to skip the current char; we don't want to test it
    // again in the subsequent function.
    cur_pos = next_pos;
    int end_of_word =
        SkipTheRemainingOfWordAndTrailingWhiteSpaces(input_text, cur_pos);

    // The current character is not a word boundary. The case is simple: We are
    // at the start or middle of some word with unknown characters or exceeding
    // the length limit. We map the entire word unk_token, skip the remaining
    // portion, and continue.
    ResetOutputAppendUnknownToken<kGetPieces, kGetIds, kGetOffsets>(
        input_word_offset_in_text, (end_of_word - input_word_offset_in_text),
        original_num_tokens, output_pieces, output_ids, output_start_offsets,
        output_end_offsets);
  }
}
// This function implements the new linear WordPiece algorithm. The overall
// design is illustrated as follows:
//
//  * WordPiece tokenization works in a left-to-right longest-matching-first
//  greedy manner, known as maximum matching.
//
//  * We use a trie containing all pieces from the vocabulary.
//
//  * We iterate the input text left-to-right, following the trie in search of
//  longer and longer matches.
//
//  * Challenge: When we fall off the trie matching, the best match is usually
//  several characters back.
//
//    * For example, assume the vocabulary is {a, ab, ##cd, ##efz, abcdefg}.
//    If the input is "abcdefz", the trie matching stops at the position of
//    "z". However, the longest match is "ab", which is 5 characters back.
//
//  * Straightforward solution: Remember the last match while iterating on the
//  trie. That gives us the longest match. Then we roll our string iterator
//  backwards and reprocess the characters that weren't part of the match. It
//  can be proved that the time complexity is quadratic.
//
//    * For the example above, it will backtrack to the 3rd position and
//    restart matching from "c", resulting in repetitive, wasteful iterations.
//
//  * Optimized solution (the novel linear algorithm): Instead of having to
//  reprocess the letters that didn't match, we can have the trie record
//  (1) the longest-matching tokens that we would have identified (called
//  "failure pops") and (2) a link pointing to a node (called "failure link")
//  representing the state from where we can continue to match the next
//  character. When trie matching cannot consume an input character, we perform
//  a "failure transition" by (a) appending the failure pops to the tokenization
//  result and (b) transiting through the failure link to a new state to
//  continue the process. Our string iterator never backtracks, and it can be
//  proved that we make at most `n` failure transitions in total in processing a
//  string of length `n`. Therefore, the time complexity is linear.
//
//    * For the same example above, when the trie matching fails at the
//    character "z", the optimized solution is smart enough to know that the
//    longest-matching tokens we can collect are ["ab", "##cd"]. It is also
//    smart enough to set itself into such a state as if it has only seen and
//    matched "##ef" so far. Now given the next character being "z", it
//    immediately identifies the next matching token as "##efz".
template <bool kGetPieces, bool kGetIds, bool kGetOffsets>
void FastWordpieceTokenizer::TokenizeSingleWordImpl(
    absl::string_view input_word, int input_word_offset_in_text,
    std::vector<std::string>* output_pieces, std::vector<int>* output_ids,
    std::vector<int>* output_start_offsets,
    std::vector<int>* output_end_offsets) const {
  static_assert(kGetPieces || kGetIds,
                "At least one of `kGetPieces` and `kGetIds` should be true.");
  if (input_word.empty()) {
    return;
  }
  const int input_size = input_word.size();

  // `original_num_tokens` stores the number of tokens in the output before
  // tokenizing this `input_word`. This is needed because we attempt to tokenize
  // `input_word` into word piece tokens and append the recognized tokens to the
  // outputs on the fly. If we later find out that `input_word` cannot be
  // tokenized into sub-tokens with the current vocabulary, we roll-back the
  // output vectors (by removing those tentative tokens) based on
  // `original_num_tokens` and appends the "unk_token".
  int original_num_tokens =
      GetCurrentOutputSize<kGetPieces>(output_pieces, output_ids);

  if (input_word.size() > config_->max_bytes_per_token()) {
    ResetOutputAppendUnknownToken<kGetPieces, kGetIds, kGetOffsets>(
        input_word_offset_in_text, input_size, original_num_tokens,
        output_pieces, output_ids, output_start_offsets, output_end_offsets);
    return;
  }

  // `cur_offset_in_input_word` tracks the offset of the remaining portion of
  // `input_word`, for which the tokens are yet to be recognized and outputted.
  // Initially it just points to the start of the input. And it gets moved
  // when more tokens are outputed.
  //
  // For example, suppose the vocab is {a,abcd,##b,##bc,##z}, and the input is
  // "abcz". First `cur_offset_in_input_word` points to position 0, since we
  // haven't ouputted any tokens. After the first token "a" is recognized and
  // outputted, it moves passing the substring "a" to position 1. Then after the
  // second token "##bc" is recognized and put to the outputs, it moves passing
  // the substring "bc" to position 3.
  //
  // This variable is used to calculate the offsets of each word piece token.
  // And since knowing their offsets in the input word, we're also able to get
  // the token string without looking it up in the vocabulary table. This saves
  // an extra look-up in hash table (saving time), and we don't even need to
  // save the vocabulary table anymore (saving memory).
  int cur_offset_in_input_word = 0;

  // Here is an example to illustrate the inference process.
  //
  // Suppose the vocabulary is {a,abcd,##b,##bc,##z}, and the suffix indicator
  // is ##. Below is the trie built from that vocabulary:
  //
  //        (a)     (b)     (c)     (d)
  //     0 ----- 3 ----- 4 ----- 5 ----- 6
  //  (#)|
  //     1
  //  (#)|  (b)     (c)
  //     2 ----- 7 ----- 8
  //     |  (z)
  //     + ----- 9
  //
  // The algorithm constructs auxiliary structures on top of the trie to enable
  // linear inference, which consist of two parts (let v denote a node):
  // * failure links f(v), pointing to another node,
  // * failure pops F(v), a list of tokens stored on node v.
  //
  // The table of str(v) (which is the string along the trie path from the root
  // to node v), f(v), and F(v) for the above trie is as follows:
  //
  //     v |    0     1     2     3     4     5       6      7       8      9
  // str(v)|   ""     #    ##     a    ab   abc    abcd    ##b    ##bc    ##z
  //   F(v)|   []    []    []   [a]   [a]   [a]  [abcd]  [##b]  [##bc]  [##z]
  //   f(v)| null  null  null     2     7     8       2      2      2    null
  //
  // Please refer to `FastWordpieceTokenizerBuilder.h|cc` for detailed
  // information on how failure links and failure pops are constructed.
  //
  // Let the input word be "abcz". Below is the inference process that is
  // carried out by this method.
  //
  //  Step | Char |  Node transition |          Output
  //     0 |      |                0 |              []
  //     1 |   a  |   goto(0,a) -> 3 |              []
  //     2 |   b  |   goto(3,b) -> 4 |              []
  //     3 |   c  |   goto(4,c) -> 5 |              []
  //     4 |   z  |        f(5) -> 8 |             [a]
  //       |   z  |        f(8) -> 2 |       [a, ##bc]
  //       |   z  |   goto(2,z) -> 9 |       [a, ##bc]
  //     final    |        f(9) -> 2 |  [a, ##bc, ##z]
  //
  // Notes:
  // * In each step we match and process one input character.
  // * goto(u,c) -> v: following the trie link with label c to transit from node
  //   u to node v.
  // * f(u) -> v: following the failure link to transit from node u to node v.
  // * The "final" step means that after processing all input characters, we
  //   keep transiting through the failure links until arriving at the node 2
  //   that represents the suffix indicator "##".
  //
  // Please refer to the below code and comments.

  // Start from the root of the trie.
  auto cur_node = trie_->CreateTraversalCursorPointToRoot();

  for (auto ch : input_word) {
    // Although the matching is on Unicode codepoints, it is equivalent to
    // directly work with the utf-8 encoding bytes.
    while (!trie_->TryTraverseOneStep(cur_node, ch)) {
      // Trie cannot consume `ch`. As explained earlier (see "Optimized
      // solution" above) we need to (1) pop one or more longest-matching tokens
      // (i.e., failure pops) off the start of the string represented by the
      // current node, and (2) transit through the failure link to a node that
      // represents the remaining suffix string after popping those
      // longest-matching prefix tokens.
      if (!TryFollowFailureLinkAndCollectTokens<kGetPieces, kGetIds,
                                                kGetOffsets>(
              input_word, input_word_offset_in_text, cur_offset_in_input_word,
              cur_node, output_pieces, output_ids, output_start_offsets,
              output_end_offsets)) {
        // If unable to follow the failure link, it means that the current trie
        // node doesn't have any matching prefix vocab tokens to pop. Since the
        // next character is not associated with a valid trie edge, the entire
        // word cannot be tokenized.
        ResetOutputAppendUnknownToken<kGetPieces, kGetIds, kGetOffsets>(
            input_word_offset_in_text, input_size, original_num_tokens,
            output_pieces, output_ids, output_start_offsets,
            output_end_offsets);
        return;
      }
    }
    // Trie consumed `ch` and was able to traverse to a new node. Continue and
    // process the next character.
  }
  // Segment the remaining string on the trie into tokens and collect them, or
  // determine that the word cannot be tokenized.
  HandleTheRemainingStringOnTriePath<kGetPieces, kGetIds, kGetOffsets>(
      input_word, input_word_offset_in_text, cur_node, original_num_tokens,
      cur_offset_in_input_word, output_pieces, output_ids, output_start_offsets,
      output_end_offsets);
}

template <bool kGetPieces, bool kGetIds, bool kGetOffsets>
ABSL_ATTRIBUTE_ALWAYS_INLINE bool
FastWordpieceTokenizer::TryFollowFailureLinkAndCollectTokens(
    absl::string_view input_word, int input_word_offset_in_text,
    int& cur_offset_in_input_word,
    trie_utils::DartsCloneTrieWrapper::TraversalCursor& node,
    std::vector<std::string>* output_pieces, std::vector<int>* output_ids,
    std::vector<int>* output_start_offsets,
    std::vector<int>* output_end_offsets) const {
  int cur_node_data;
  if (trie_->TryGetData(node, cur_node_data)) {
    // A shortcut to get f(cur_node) (i.e., the failure link) and F(cur_node)
    // (i.e., failure pops) when `cur_node` has data. This results in ~10%
    // speedup (statistically significant).
    AppendTokenToOutput<kGetPieces, kGetIds, kGetOffsets>(
        input_word, input_word_offset_in_text, cur_offset_in_input_word,
        cur_node_data, output_pieces, output_ids, output_start_offsets,
        output_end_offsets);
    // Transit through the failure link.
    trie_->SetTraversalCursor(
        node,
        config_->failure_struct_array()->Get(node.node_id)->failure_link());
    return true;
  }

  const auto& node_aux = config_->failure_struct_array()->Get(node.node_id);

  if (node_aux->failure_link() == fast_wordpiece_tokenizer_utils::kNullNode) {
    // No failure_link can be followed.
    return false;
  }

  // Collect the tokens (i.e., failure pops), represented by (offset, length) in
  // a failure_pops pool (held by the config flatbuffer).
  int failure_pops_offset, failure_pops_length;
  fast_wordpiece_tokenizer_utils::GetFailurePopsOffsetAndLength(
      node_aux->failure_pops_offset_length(), failure_pops_offset,
      failure_pops_length);
  const int failure_pops_end_offset = failure_pops_offset + failure_pops_length;
  for (int offset_in_pool = failure_pops_offset;
       offset_in_pool < failure_pops_end_offset; ++offset_in_pool) {
    AppendTokenToOutput<kGetPieces, kGetIds, kGetOffsets>(
        input_word, input_word_offset_in_text, cur_offset_in_input_word,
        config_->failure_pops_pool()->Get(offset_in_pool), output_pieces,
        output_ids, output_start_offsets, output_end_offsets);
  }

  // Transit through the failure link.
  trie_->SetTraversalCursor(node, node_aux->failure_link());
  return true;
}

template <bool kGetPieces, bool kGetIds, bool kGetOffsets>
void FastWordpieceTokenizer::AppendTokenToOutput(
    absl::string_view input_word, int input_word_offset_in_text,
    int& cur_offset_in_input_word, int encoded_token_value,
    std::vector<std::string>* output_pieces, std::vector<int>* output_ids,
    std::vector<int>* output_start_offsets,
    std::vector<int>* output_end_offsets) const {
  auto token_id =
      fast_wordpiece_tokenizer_utils::GetTokenId(encoded_token_value);
  if constexpr (kGetIds) {
    output_ids->push_back(token_id);
  }
  if constexpr (kGetPieces || kGetOffsets) {
    // For suffix tokens, the length below is without the suffix indicator.
    int token_substr_length =
        fast_wordpiece_tokenizer_utils::GetTokenLength(encoded_token_value);
    if (!cur_offset_in_input_word &&
        fast_wordpiece_tokenizer_utils::IsSuffixToken(encoded_token_value)) {
      // This is a special case where `input_word` happens to start with the
      // suffix indicator (e.g., "##") and a suffix token is recognized at the
      // start (since `cur_offset_input_word == 0`). In this case, we need
      // to adjust and add the length of the suffix indicator string.
      token_substr_length += config_->suffix_indicator()->size();
    }
    if constexpr (kGetPieces) {
      // If token id is unk_token_id, it means that it is a dummy node for
      // punctuations that are not contained in the vocabulary, we append
      // the unk_token in this case. Otherwise, we
      // get the subword string from `input_word` by the offset and length.
      auto unk_token = config_->unk_token()->string_view();
      auto subword_str =
          (token_id == config_->unk_token_id())
              ? absl::string_view(unk_token.data(), unk_token.size())
              : absl::string_view(input_word.data() + cur_offset_in_input_word,
                                  token_substr_length);
      output_pieces->emplace_back(
          cur_offset_in_input_word
              ? absl::StrCat(config_->suffix_indicator()->str(), subword_str)
              : subword_str);
    }
    if constexpr (kGetOffsets) {
      // Record the offsets relative to the start of the whole text.
      output_start_offsets->push_back(input_word_offset_in_text +
                                      cur_offset_in_input_word);
      output_end_offsets->push_back(input_word_offset_in_text +
                                    cur_offset_in_input_word +
                                    token_substr_length);
    }
    cur_offset_in_input_word += token_substr_length;
  }
}

template <bool kGetPieces, bool kGetIds, bool kGetOffsets>
ABSL_ATTRIBUTE_ALWAYS_INLINE void
FastWordpieceTokenizer::HandleTheRemainingStringOnTriePath(
    absl::string_view input_word, int input_word_offset_in_text,
    trie_utils::DartsCloneTrieWrapper::TraversalCursor& cur_node,
    int& original_num_tokens, int& cur_offset_in_input_word,
    std::vector<std::string>* output_pieces, std::vector<int>* output_ids,
    std::vector<int>* output_start_offsets,
    std::vector<int>* output_end_offsets) const {
  if (cur_node.node_id == trie_utils::DartsCloneTrieWrapper::kRootNodeId) {
    // We've seen an empty input word. Just return.
    return;
  }
  // Try handling the special case where the entire input word happens to be the
  // suffix indicator (e.g., "##") itself.
  if (TryHandleTheInputWordBeingSuffixIndicatorItself<kGetPieces, kGetIds,
                                                      kGetOffsets>(
          input_word, input_word_offset_in_text, cur_node,
          cur_offset_in_input_word, original_num_tokens, output_pieces,
          output_ids, output_start_offsets, output_end_offsets)) {
    original_num_tokens =
        GetCurrentOutputSize<kGetPieces>(output_pieces, output_ids);
    return;
  }

  // Handle the normal case because we need to collect the remaining tokens from
  // the string represented by `cur_node` (i.e., on the trie path from the trie
  // root to `cur_node`), or find out the word cannot be tokenized.
  //
  // See the example in the comments of this function in the header file.
  //
  // The tokenization is successful if and only if the entire string represented
  // by `cur_node` can be segmented into consecutive matching tokens, resulting
  // in the empty suffix string (e.g., "##"), which is represented by
  // `trie_suffix_root_`. So we keep following the failure links and collecting
  // failure pops tokens until we arrive at `trie_suffix_root_` or encounter a
  // null failure link in the middle.
  while (cur_node.node_id != config_->trie_suffix_root() &&
         cur_node.node_id != config_->trie_punct_failure_link_node()) {
    if (!TryFollowFailureLinkAndCollectTokens<kGetPieces, kGetIds, kGetOffsets>(
            input_word, input_word_offset_in_text, cur_offset_in_input_word,
            cur_node, output_pieces, output_ids, output_start_offsets,
            output_end_offsets)) {
      // The remaining string cannot be tokenized, neither can the input word.
      ResetOutputAppendUnknownToken<kGetPieces, kGetIds, kGetOffsets>(
          input_word_offset_in_text, input_word.size(), original_num_tokens,
          output_pieces, output_ids, output_start_offsets, output_end_offsets);
      return;
    }
  }
  // Arrive at `trie_suffix_root_`.

  // Update the `original_num_tokens`.
  original_num_tokens =
      GetCurrentOutputSize<kGetPieces>(output_pieces, output_ids);

  // Succeed and exit.
}

template <bool kGetPieces, bool kGetIds, bool kGetOffsets>
void FastWordpieceTokenizer::ResetOutputAppendUnknownToken(
    int input_word_offset_in_text, int input_size, int& original_num_tokens,
    std::vector<std::string>* output_pieces, std::vector<int>* output_ids,
    std::vector<int>* output_start_offsets,
    std::vector<int>* output_end_offsets) const {
  if constexpr (kGetPieces) {
    output_pieces->resize(original_num_tokens + 1);
    output_pieces->back() = config_->unk_token()->str();
  }
  if constexpr (kGetIds) {
    output_ids->resize(original_num_tokens + 1);
    output_ids->back() = config_->unk_token_id();
  }
  if constexpr (kGetOffsets) {
    output_start_offsets->resize(original_num_tokens + 1);
    output_start_offsets->back() = input_word_offset_in_text;

    output_end_offsets->resize(original_num_tokens + 1);
    output_end_offsets->back() = input_word_offset_in_text + input_size;
  }

  // Update `original_num_tokens` (since we have appended the "unk_token").
  ++original_num_tokens;
}

template <bool kGetPieces, bool kGetIds, bool kGetOffsets>
ABSL_ATTRIBUTE_ALWAYS_INLINE bool
FastWordpieceTokenizer::TryHandleTheInputWordBeingSuffixIndicatorItself(
    absl::string_view input_word, int input_word_offset_in_text,
    const trie_utils::DartsCloneTrieWrapper::TraversalCursor& cur_node,
    int& cur_offset_in_input_word, int original_num_tokens,
    std::vector<std::string>* output_pieces, std::vector<int>* output_ids,
    std::vector<int>* output_start_offsets,
    std::vector<int>* output_end_offsets) const {
  // Handle the special case where the input word is the suffix indicator (e.g.,
  // "##") itself. This is because that, after all the characters of an input
  // word were successfully processed, if we ended by standing at
  // `trie_suffix_root_` but did not recognize any new tokens, it can only be
  // the case that the word is the suffix indicator string (e.g., "##") itself.
  // For this case we output the pre-computed result.
  if (cur_node.node_id != config_->trie_suffix_root()) {
    // The input word is not the suffix indicator itself.
    return false;
  }
  int cur_num_tokens =
      GetCurrentOutputSize<kGetPieces>(output_pieces, output_ids);
  if (cur_num_tokens != original_num_tokens) {
    // The input word is not the suffix indicator itself.
    return false;
  }

  // The input word is the suffix indicator itself. Next we handle two cases.
  if (config_->precomputed_result_for_suffix_indicator()->size() == 1 &&
      fast_wordpiece_tokenizer_utils::GetTokenId(
          config_->precomputed_result_for_suffix_indicator()->Get(0)) ==
          config_->unk_token_id()) {
    // Case 1: The suffix indicator string cannot be tokenized but has to be
    // mapped to unk_token.
    ResetOutputAppendUnknownToken<kGetPieces, kGetIds, kGetOffsets>(
        input_word_offset_in_text, input_word.size(), original_num_tokens,
        output_pieces, output_ids, output_start_offsets, output_end_offsets);
    return true;
  }

  // Case 2: The suffix indicator can be tokenized normally.
  for (int encoded_token_value :
       *config_->precomputed_result_for_suffix_indicator()) {
    AppendTokenToOutput<kGetPieces, kGetIds, kGetOffsets>(
        input_word, input_word_offset_in_text, cur_offset_in_input_word,
        encoded_token_value, output_pieces, output_ids, output_start_offsets,
        output_end_offsets);
  }
  return true;
}
}  // namespace text
}  // namespace tensorflow
