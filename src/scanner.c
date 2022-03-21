#include <assert.h>
#include <string.h>
#include <tree_sitter/parser.h>

enum TokenType {
  VIRTUAL_END_DECL,
  VIRTUAL_OPEN_SECTION,
  VIRTUAL_END_SECTION,
  MINUS_WITHOUT_TRAILING_WHITESPACE,
  GLSL_CONTENT,
  BLOCK_COMMENT_CONTENT
};

// The maximum amount of indentation frames we can store is equal to the
// maximum size tree sitter allows for the scanner, minus all the other scanner
// fields, divided by 4 because each indentation frame requires 4 bytes.
#define TREE_SITTER_ELM_MAX_INDENT_FRAMES                                      \
  (TREE_SITTER_SERIALIZATION_BUFFER_SIZE - sizeof(uint32_t) -                  \
   2 * sizeof(uint8_t) - sizeof(uint64_t)) /                                   \
      sizeof(uint32_t)

struct Scanner {
  // The indention of the current line
  uint32_t indent_length;
  // Our indentation stack
  uint32_t indent_stack[TREE_SITTER_ELM_MAX_INDENT_FRAMES];
  // Size of the previous indentation stack
  uint8_t indent_stack_count;
  // Bitfield with 0 - for possible VIRTUAL_END_DECL or 1 - for possible
  // VIRTUAL_END_SECTION
  uint64_t runback;
  // Size of previous bitfield
  uint8_t runback_count;
};

void *tree_sitter_elm_external_scanner_create() {
  struct Scanner *scanner = malloc(sizeof(*scanner));
  scanner->indent_length = 0;
  scanner->indent_stack[0] = 0;
  scanner->indent_stack_count = 1;
  scanner->runback = 0;
  scanner->runback_count = 0;
  return scanner;
}

void tree_sitter_elm_external_scanner_destroy(void *payload) {
  struct Scanner *scanner = (payload);
  free(scanner);
}

unsigned tree_sitter_elm_external_scanner_serialize(void *payload,
                                                    char *buffer) {
  struct Scanner *scanner = (payload);
  size_t i = 0;

  buffer[i++] = scanner->runback_count;
  memcpy(&buffer[i], &scanner->runback, sizeof(uint64_t));
  i += sizeof(uint64_t);

  memcpy(&buffer[i], &scanner->indent_length, sizeof(uint32_t));
  i += sizeof(uint32_t);

  buffer[i++] = scanner->indent_stack_count;
  size_t indent_stack_length = scanner->indent_stack_count * 4;
  memcpy(&buffer[i], &scanner->indent_stack, indent_stack_length);
  i += indent_stack_length;

  return i;
}

void tree_sitter_elm_external_scanner_deserialize(void *payload,
                                                  const char *buffer,
                                                  unsigned length) {
  struct Scanner *scanner = (payload);
  scanner->runback_count = 0;
  scanner->runback = 0;
  scanner->indent_stack[0] = 0;
  scanner->indent_stack_count = 1;

  if (length > 0) {
    size_t i = 0;

    scanner->runback_count = (uint8_t)buffer[i++];
    memcpy(&scanner->runback, &buffer[i], sizeof(uint64_t));
    i += sizeof(uint64_t);

    memcpy(&scanner->indent_length, &buffer[i], sizeof(uint32_t));
    i += sizeof(uint32_t);

    scanner->indent_stack_count = buffer[i++];
    size_t indent_stack_length = scanner->indent_stack_count * 4;
    memcpy(&scanner->indent_stack, &buffer[i], indent_stack_length);
  }
}

void ts_elm_advance(TSLexer *lexer) { lexer->advance(lexer, false); }

void ts_elm_skip(TSLexer *lexer) { lexer->advance(lexer, true); }

uint32_t ts_elm_indent_stack_read_back(struct Scanner *scanner) {
  assert(scanner->indent_stack_count > 0);
  return scanner->indent_stack[scanner->indent_stack_count - 1];
}

uint32_t ts_elm_indent_stack_pop_back(struct Scanner *scanner) {
  uint32_t indent = ts_elm_indent_stack_read_back(scanner);
  scanner->indent_stack_count -= 1;
  return indent;
}

void ts_elm_indent_stack_push_back(struct Scanner *scanner, uint32_t indent) {
  assert(scanner->indent_stack_count < TREE_SITTER_ELM_MAX_INDENT_FRAMES);
  scanner->indent_stack[scanner->indent_stack_count] = indent;
  scanner->indent_stack_count += 1;
}

enum TokenType ts_elm_runback_read_back(struct Scanner *scanner) {
  assert(scanner->runback_count > 0);
  bool bit = (scanner->runback >> (scanner->runback_count - 1)) & 1U;
  if (bit) {
    return VIRTUAL_END_SECTION;
  } else {
    return VIRTUAL_END_DECL;
  }
}

enum TokenType runback_pop_back(struct Scanner *scanner) {
  enum TokenType last = ts_elm_runback_read_back(scanner);
  scanner->runback_count -= 1;
  return last;
}

void ts_elm_runback_push_front(struct Scanner *scanner, enum TokenType elem) {
  assert(scanner->indent_stack_count < 64);
  scanner->runback <<= 1;
  if (elem == VIRTUAL_END_SECTION) {
    scanner->runback |= 1ULL;
  }
  scanner->runback_count += 1;
}

bool ts_elm_runback_empty(struct Scanner *scanner) {
  return scanner->runback_count == 0;
}

void ts_elm_runback_clear(struct Scanner *scanner) {
  scanner->runback_count = 0;
}

bool ts_elm_is_elm_space(TSLexer *lexer) {
  return lexer->lookahead == ' ' || lexer->lookahead == '\r' ||
         lexer->lookahead == '\n';
}

int ts_elm_check_for_in(TSLexer *lexer, const bool *valid_symbols) {
  // Are we at the end of a let (in) declaration
  if (valid_symbols[VIRTUAL_END_SECTION] && lexer->lookahead == 'i') {
    ts_elm_skip(lexer);

    if (lexer->lookahead == 'n') {
      ts_elm_skip(lexer);
      if (ts_elm_is_elm_space(lexer) || lexer->eof(lexer)) {
        return 2; // Success
      }
      return 1; // Partial
    }
    return 1; // Partial
  }
  return 0;
}

bool ts_elm_scan_block_comment(TSLexer *lexer) {
  lexer->mark_end(lexer);
  if (lexer->lookahead != '{')
    return false;

  ts_elm_advance(lexer);
  if (lexer->lookahead != '-')
    return false;

  ts_elm_advance(lexer);

  while (true) {
    switch (lexer->lookahead) {
    case '{':
      ts_elm_scan_block_comment(lexer);
      break;
    case '-':
      ts_elm_advance(lexer);
      if (lexer->lookahead == '}') {
        ts_elm_advance(lexer);
        return true;
      }
      break;
    case '\0':
      return true;
    default:
      ts_elm_advance(lexer);
    }
  }
}

void ts_elm_advance_to_line_end(TSLexer *lexer) {
  while (true) {
    if (lexer->lookahead == '\n') {
      break;
    } else if (lexer->eof(lexer)) {
      break;
    } else {
      ts_elm_advance(lexer);
    }
  }
}

bool tree_sitter_elm_external_scanner_scan(void *payload, TSLexer *lexer,
                                           const bool *valid_symbols) {
  struct Scanner *scanner = (payload);
  // First handle eventual runback tokens, we saved on a previous scan op
  if (!ts_elm_runback_empty(scanner) &&
      valid_symbols[ts_elm_runback_read_back(scanner)]) {
    lexer->result_symbol = runback_pop_back(scanner);
    return true;
  }
  ts_elm_runback_clear(scanner);

  // Check if we have newlines and how much indentation
  bool has_newline = false;
  bool found_in = false;
  bool can_call_mark_end = true;
  lexer->mark_end(lexer);
  while (true) {
    if (lexer->lookahead == ' ') {
      ts_elm_skip(lexer);
    } else if (lexer->lookahead == '\n') {
      ts_elm_skip(lexer);
      has_newline = true;
      while (true) {
        if (lexer->lookahead == ' ') {
          ts_elm_skip(lexer);
        } else {
          scanner->indent_length = lexer->get_column(lexer);
          break;
        }
      }
    } else if (!valid_symbols[BLOCK_COMMENT_CONTENT] &&
               lexer->lookahead == '-') {

      ts_elm_advance(lexer);
      uint32_t lookahead = lexer->lookahead;

      // Handle minus without a whitespace for negate
      if (valid_symbols[MINUS_WITHOUT_TRAILING_WHITESPACE] &&
          ((lookahead >= 'a' && lookahead <= 'z') ||
           (lookahead >= 'A' && lookahead <= 'Z') || lookahead == '(')) {
        if (can_call_mark_end) {
          lexer->result_symbol = MINUS_WITHOUT_TRAILING_WHITESPACE;
          lexer->mark_end(lexer);
          return true;
        } else {
          return false;
        }
      }
      // Scan past line comments. As far as the special token
      // types we're scanning for here are concerned line comments
      // are like whitespace. There is nothing useful to be
      // learned from, say, their indentation. So we advance past
      // them here.
      //
      // The one thing we need to keep in mind is that we should
      // not call `lexer->mark_end(lexer)` after this point, or
      // the comment will be lost.
      else if (lookahead == '-' && has_newline) {
        can_call_mark_end = false;
        ts_elm_advance(lexer);
        ts_elm_advance_to_line_end(lexer);
      } else if (valid_symbols[BLOCK_COMMENT_CONTENT] &&
                 lexer->lookahead == '}') {
        lexer->result_symbol = BLOCK_COMMENT_CONTENT;
        return true;
      } else {
        return false;
      }
    } else if (lexer->lookahead == '\r') {
      ts_elm_skip(lexer);
    } else if (lexer->eof(lexer)) {
      if (valid_symbols[VIRTUAL_END_SECTION]) {
        lexer->result_symbol = VIRTUAL_END_SECTION;
        return true;
      }
      if (valid_symbols[VIRTUAL_END_DECL]) {
        lexer->result_symbol = VIRTUAL_END_DECL;
        return true;
      }

      break;
    } else {
      break;
    }
  }

  if (ts_elm_check_for_in(lexer, valid_symbols) == 2) {
    if (has_newline) {
      found_in = true;
    } else {
      lexer->result_symbol = VIRTUAL_END_SECTION;
      ts_elm_indent_stack_pop_back(scanner);
      return true;
    }
  }

  // Open section if the grammar lets us but only push to indent stack if we
  // go further down in the stack
  if (valid_symbols[VIRTUAL_OPEN_SECTION] && !lexer->eof(lexer)) {
    ts_elm_indent_stack_push_back(scanner, lexer->get_column(lexer));
    lexer->result_symbol = VIRTUAL_OPEN_SECTION;
    return true;
  } else if (valid_symbols[BLOCK_COMMENT_CONTENT]) {
    if (!can_call_mark_end) {
      return false;
    }
    lexer->mark_end(lexer);
    while (true) {
      if (lexer->lookahead == '\0') {
        break;
      }
      if (lexer->lookahead != '{' && lexer->lookahead != '-') {
        ts_elm_advance(lexer);
      } else if (lexer->lookahead == '-') {
        lexer->mark_end(lexer);
        ts_elm_advance(lexer);
        if (lexer->lookahead == '}') {
          break;
        }
      } else if (ts_elm_scan_block_comment(lexer)) {
        lexer->mark_end(lexer);
        ts_elm_advance(lexer);
        if (lexer->lookahead == '-') {
          break;
        }
      }
    }

    lexer->result_symbol = BLOCK_COMMENT_CONTENT;
    return true;
  } else if (has_newline) {
    // We had a newline now it's time to check if we need to add multiple
    // tokens to get back up to the right level
    ts_elm_runback_clear(scanner);

    while (scanner->indent_length <= ts_elm_indent_stack_read_back(scanner)) {
      if (scanner->indent_length == ts_elm_indent_stack_read_back(scanner)) {
        if (found_in) {
          ts_elm_runback_push_front(scanner, VIRTUAL_END_SECTION);
          found_in = false;
          break;
        }
        // Don't insert VIRTUAL_END_DECL when there is a line comment incoming

        if (lexer->lookahead == '-') {
          ts_elm_skip(lexer);
          if (lexer->lookahead == '-') {
            break;
          }
        }
        // Don't insert VIRTUAL_END_DECL when there is a block comment
        // incoming
        if (lexer->lookahead == '{') {
          ts_elm_skip(lexer);
          if (lexer->lookahead == '-') {
            break;
          }
        }
        ts_elm_runback_push_front(scanner, VIRTUAL_END_DECL);
        break;
      } else if (scanner->indent_length <
                 ts_elm_indent_stack_read_back(scanner)) {
        ts_elm_indent_stack_pop_back(scanner);
        ts_elm_runback_push_front(scanner, VIRTUAL_END_SECTION);
        found_in = false;
      }
    }

    // Needed for some of the more weird cases where let is in the same line
    // as everything before the in in the next line
    if (found_in) {
      ts_elm_runback_push_front(scanner, VIRTUAL_END_SECTION);
      found_in = false;
    }

    // Handle the first runback token if we have them, if there are more they
    // will be handled on the next scan operation
    if (!ts_elm_runback_empty(scanner) &&
        valid_symbols[ts_elm_runback_read_back(scanner)]) {
      lexer->result_symbol = runback_pop_back(scanner);
      return true;
    } else if (lexer->eof(lexer) && valid_symbols[VIRTUAL_END_SECTION]) {
      lexer->result_symbol = VIRTUAL_END_SECTION;
      return true;
    }
  }

  if (valid_symbols[GLSL_CONTENT]) {
    if (!can_call_mark_end) {
      return false;
    }
    lexer->result_symbol = GLSL_CONTENT;
    while (true) {
      switch (lexer->lookahead) {
      case '|':
        lexer->mark_end(lexer);
        ts_elm_advance(lexer);
        if (lexer->lookahead == ']') {
          ts_elm_advance(lexer);
          return true;
        }
        break;
      case '\0':
        lexer->mark_end(lexer);
        return true;
      default:
        ts_elm_advance(lexer);
      }
    }
  }

  return false;
}
