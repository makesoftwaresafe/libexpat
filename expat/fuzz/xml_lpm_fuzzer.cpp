/*
                            __  __            _
                         ___\ \/ /_ __   __ _| |_
                        / _ \\  /| '_ \ / _` | __|
                       |  __//  \| |_) | (_| | |_
                        \___/_/\_\ .__/ \__,_|\__|
                                 |_| XML parser

   Copyright (c) 2022      Google LLC
   Licensed under the MIT license:

   Permission is  hereby granted,  free of charge,  to any  person obtaining
   a  copy  of  this  software   and  associated  documentation  files  (the
   "Software"),  to  deal in  the  Software  without restriction,  including
   without  limitation the  rights  to use,  copy,  modify, merge,  publish,
   distribute, sublicense, and/or sell copies of the Software, and to permit
   persons  to whom  the Software  is  furnished to  do so,  subject to  the
   following conditions:

   The above copyright  notice and this permission notice  shall be included
   in all copies or substantial portions of the Software.

   THE  SOFTWARE  IS  PROVIDED  "AS  IS",  WITHOUT  WARRANTY  OF  ANY  KIND,
   EXPRESS  OR IMPLIED,  INCLUDING  BUT  NOT LIMITED  TO  THE WARRANTIES  OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
   NO EVENT SHALL THE AUTHORS OR  COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
   DAMAGES OR  OTHER LIABILITY, WHETHER  IN AN  ACTION OF CONTRACT,  TORT OR
   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
   USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#if defined(NDEBUG)
#  undef NDEBUG  // because checks below rely on assert(...)
#endif

#include <assert.h>
#include <stdint.h>
#include <vector>

#include "expat.h"
#include "xml_lpm_fuzzer.pb.h"
#include "src/libfuzzer/libfuzzer_macro.h"

static const char* g_encoding = nullptr;
static const char* external_entity = nullptr;
static size_t external_entity_size = 0;

void SetEncoding(const xml_lpm_fuzzer::Encoding& e) {
  switch (e) {
    case xml_lpm_fuzzer::Encoding::UTF8:
      g_encoding = "UTF-8";
      break;

    case xml_lpm_fuzzer::Encoding::UTF16:
      g_encoding = "UTF-16";
      break;

    case xml_lpm_fuzzer::Encoding::ISO88591:
      g_encoding = "ISO-8859-1";
      break;

    case xml_lpm_fuzzer::Encoding::ASCII:
      g_encoding = "US-ASCII";
      break;

    case xml_lpm_fuzzer::Encoding::NONE:
      g_encoding = NULL;
      break;

    default:
      g_encoding = "UNKNOWN";
      break;
  }
}

static int allocation_count = 0;
static std::vector<int> fail_allocations = {};

void* MallocHook(size_t size) {
  for (auto index : fail_allocations) {
    if (index == allocation_count) {
      return NULL;
    }
  }
  allocation_count += 1;
  return malloc(size);
}

void* ReallocHook(void* ptr, size_t size) {
  for (auto index : fail_allocations) {
    if (index == allocation_count) {
      return NULL;
    }
  }
  allocation_count += 1;
  return realloc(ptr, size);
}

void FreeHook(void* ptr) {
  free(ptr);
}

XML_Memory_Handling_Suite memory_handling_suite = {
  MallocHook, ReallocHook, FreeHook
};

void InitializeParser(XML_Parser parser);

// We want a parse function that supports resumption, so that we can cover the
// suspend/resume code.
enum XML_Status Parse(XML_Parser parser, const XML_Char* input, int input_len,
                      int is_final) {
  enum XML_Status status = XML_Parse(parser, input, input_len, is_final);
  while (status == XML_STATUS_SUSPENDED) {
    status = XML_ResumeParser(parser);
  }
  return status;
}

// When the fuzzer is compiled with instrumentation such as ASan, then the
// accesses in TouchString will fault if they access invalid memory (ie. detect
// either a use-after-free or buffer-overflow). By calling TouchString in each
// of the callbacks, we can check that the arguments meet the API specifications
// in terms of length/null-termination. no_optimize is used to ensure that the
// compiler has to emit actual memory reads, instead of removing them.
static volatile size_t no_optimize = 0;
static void TouchString(const XML_Char* ptr, int len=-1) {
  if (!ptr) {
    return;
  }

  if (len == -1) {
    for (XML_Char value = *ptr++; value; value = *ptr++) {
      no_optimize += value;
    }
  } else {
    for (int i = 0; i < len; ++i) {
      no_optimize += ptr[i];
    }
  }
}

static void TouchChildNodes(XML_Content* content, bool topLevel=true) {
  switch (content->type) {
    case XML_CTYPE_EMPTY:
    case XML_CTYPE_ANY:
      assert(content->quant == XML_CQUANT_NONE);
      assert(content->name == NULL);
      assert(content->numchildren == 0);
      assert(content->children == NULL);
      break;

    case XML_CTYPE_MIXED:
      assert(content->quant == XML_CQUANT_NONE
             || content->quant == XML_CQUANT_REP);
      assert(content->name == NULL);
      for (int i = 0; i < content->numchildren; ++i) {
        assert(content->children[i].type == XML_CTYPE_NAME);
        assert(content->children[i].numchildren == 0);
        TouchString(content->children[i].name);
      }
      break;

    case XML_CTYPE_NAME:
      assert(content->numchildren == 0);
      TouchString(content->name);
      break;

    case XML_CTYPE_CHOICE:
    case XML_CTYPE_SEQ:
      assert(content->name == NULL);
      for (int i = 0; i < content->numchildren; ++i) {
        TouchChildNodes(&content->children[i], false);
      }
      break;

    default:
      assert(false);
  }
}

static void XMLCALL
ElementDeclHandler(void* userData, const XML_Char* name, XML_Content* model) {
  TouchString(name);
  TouchChildNodes(model);
  XML_FreeContentModel((XML_Parser)userData, model);
}

static void XMLCALL
AttlistDeclHandler(void* userData, const XML_Char* elname,
                     const XML_Char* attname, const XML_Char* atttype,
                     const XML_Char* dflt, int isrequired) {
  TouchString(elname);
  TouchString(attname);
  TouchString(atttype);
  TouchString(dflt);
}

static void XMLCALL
XmlDeclHandler(void* userData, const XML_Char* version,
               const XML_Char* encoding, int standalone) {
  TouchString(version);
  TouchString(encoding);
}

static void XMLCALL
StartElementHandler(void *userData, const XML_Char *name,
                    const XML_Char **atts) {
  TouchString(name);
  for (size_t i = 0; atts[i] != NULL; ++i) {
    TouchString(atts[i]);
  }
}

static void XMLCALL
EndElementHandler(void *userData, const XML_Char *name) {
  TouchString(name);
}

static void XMLCALL
CharacterDataHandler(void* userData, const XML_Char* s, int len) {
  TouchString(s, len);
}

static void XMLCALL
ProcessingInstructionHandler(void* userData, const XML_Char* target,
                             const XML_Char* data) {
  TouchString(target);
  TouchString(data);
}

static void XMLCALL
CommentHandler(void* userData, const XML_Char* data) {
  TouchString(data);
  // Use the comment handler to trigger parser suspend, so that we can get
  // coverage of that code.
  XML_StopParser((XML_Parser)userData, XML_TRUE);
}

static void XMLCALL
StartCdataSectionHandler(void* userData) {
}

static void XMLCALL
EndCdataSectionHandler(void* userData) {
}

static void XMLCALL
DefaultHandler(void* userData, const XML_Char* s, int len) {
  TouchString(s, len);
}

static void XMLCALL
StartDoctypeDeclHandler(void* userData, const XML_Char* doctypeName,
                        const XML_Char* sysid, const XML_Char* pubid,
                        int has_internal_subset) {
  TouchString(doctypeName);
  TouchString(sysid);
  TouchString(pubid);
}

static void XMLCALL
EndDoctypeDeclHandler(void* userData) {
}

static void XMLCALL
EntityDeclHandler(void *userData, const XML_Char *entityName,
                  int is_parameter_entity, const XML_Char *value,
                  int value_length, const XML_Char *base,
                  const XML_Char *systemId, const XML_Char *publicId,
                  const XML_Char *notationName) {
  TouchString(entityName);
  TouchString(value, value_length);
  TouchString(base);
  TouchString(systemId);
  TouchString(publicId);
  TouchString(notationName);
}

static void XMLCALL
UnparsedEntityDeclHandler(void *userData, const XML_Char *entityName,
                          const XML_Char *base, const XML_Char *systemId,
                          const XML_Char *publicId,
                          const XML_Char *notationName) {
  TouchString(entityName);
  TouchString(base);
  TouchString(systemId);
  TouchString(publicId);
  TouchString(notationName);
}

static void XMLCALL
NotationDeclHandler(void *userData, const XML_Char *notationName,
                    const XML_Char *base, const XML_Char *systemId,
                    const XML_Char *publicId) {
  TouchString(notationName);
  TouchString(base);
  TouchString(systemId);
  TouchString(publicId);
}

static void XMLCALL
StartNamespaceDeclHandler(void *userData, const XML_Char *prefix,
                          const XML_Char *uri) {
  TouchString(prefix);
  TouchString(uri);
}

static void XMLCALL
EndNamespaceDeclHandler(void *userData, const XML_Char *prefix) {
  TouchString(prefix);
}

static int XMLCALL
NotStandaloneHandler(void *userData) {
  return XML_STATUS_OK;
}

static int XMLCALL
ExternalEntityRefHandler(XML_Parser parser, const XML_Char *context,
                         const XML_Char *base, const XML_Char *systemId,
                         const XML_Char *publicId) {
  int rc = XML_STATUS_ERROR;
  TouchString(context);
  TouchString(base);
  TouchString(systemId);
  TouchString(publicId);

  if (external_entity) {
    XML_Parser ext_parser = XML_ExternalEntityParserCreate(parser, context,
                                                           g_encoding);
    rc = Parse(ext_parser, (const XML_Char*)external_entity,
               external_entity_size, 1);
    XML_ParserFree(ext_parser);
  }

  return rc;
}

static void XMLCALL
SkippedEntityHandler(void *userData, const XML_Char *entityName,
                     int is_parameter_entity) {
  TouchString(entityName);
}

static int XMLCALL
UnknownEncodingHandler(void *encodingHandlerData, const XML_Char *name,
                       XML_Encoding *info) {
  TouchString(name);
  return XML_STATUS_ERROR;
}

void InitializeParser(XML_Parser parser) {
  XML_SetUserData(parser, (void*)parser);
  XML_SetHashSalt(parser, 0x41414141);
  XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_ALWAYS);

  XML_SetElementDeclHandler(parser, ElementDeclHandler);
  XML_SetAttlistDeclHandler(parser, AttlistDeclHandler);
  XML_SetXmlDeclHandler(parser, XmlDeclHandler);
  XML_SetElementHandler(parser, StartElementHandler, EndElementHandler);
  XML_SetCharacterDataHandler(parser, CharacterDataHandler);
  XML_SetProcessingInstructionHandler(parser, ProcessingInstructionHandler);
  XML_SetCommentHandler(parser, CommentHandler);
  XML_SetCdataSectionHandler(parser, StartCdataSectionHandler,
                             EndCdataSectionHandler);
  // XML_SetDefaultHandler disables entity expansion
  XML_SetDefaultHandlerExpand(parser, DefaultHandler);
  XML_SetDoctypeDeclHandler(parser, StartDoctypeDeclHandler,
                            EndDoctypeDeclHandler);
  XML_SetEntityDeclHandler(parser, EntityDeclHandler);
  // NB: This is mutually exclusive with entity_decl_handler, and there isn't
  // any significant code change between the two.
  // XML_SetUnparsedEntityDeclHandler(p, UnparsedEntityDeclHandler);
  XML_SetNotationDeclHandler(parser, NotationDeclHandler);
  XML_SetNamespaceDeclHandler(parser, StartNamespaceDeclHandler,
                              EndNamespaceDeclHandler);
  XML_SetNotStandaloneHandler(parser, NotStandaloneHandler);
  XML_SetExternalEntityRefHandler(parser, ExternalEntityRefHandler);
  XML_SetSkippedEntityHandler(parser, SkippedEntityHandler);
  XML_SetUnknownEncodingHandler(parser, UnknownEncodingHandler, (void*)parser);
}

DEFINE_TEXT_PROTO_FUZZER(const xml_lpm_fuzzer::Testcase& testcase) {
  external_entity = nullptr;

  if (!testcase.actions_size()) {
    return;
  }

  allocation_count = 0;
  fail_allocations.clear();
  for (size_t i = 0; i < testcase.fail_allocations_size(); ++i) {
    fail_allocations.push_back(testcase.fail_allocations(i));
  }

  SetEncoding(testcase.encoding());
  XML_Parser parser = XML_ParserCreate_MM(g_encoding, &memory_handling_suite, "|");
  InitializeParser(parser);

  for (size_t i = 0; i < testcase.actions_size(); ++i) {
    const auto& action = testcase.actions(i);
    switch (action.action_case()) {
      case xml_lpm_fuzzer::Action::kChunk:
        if (XML_STATUS_ERROR == Parse(parser,
                                      (const XML_Char*)action.chunk().data(),
                                      action.chunk().size(), 0)) {
          // Force a reset after parse error.
          XML_ParserReset(parser, g_encoding);
        }
        break;

      case xml_lpm_fuzzer::Action::kLastChunk:
        Parse(parser, (const XML_Char*)action.last_chunk().data(),
              action.last_chunk().size(), 1);
        XML_ParserReset(parser, g_encoding);
        InitializeParser(parser);
        break;

      case xml_lpm_fuzzer::Action::kReset:
        XML_ParserReset(parser, g_encoding);
        InitializeParser(parser);
        break;

      case xml_lpm_fuzzer::Action::kExternalEntity:
        external_entity = action.external_entity().data();
        external_entity_size = action.external_entity().size();
        break;

      default:
        break;
    }
  }

  XML_ParserFree(parser);
}