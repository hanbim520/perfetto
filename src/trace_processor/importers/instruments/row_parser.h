/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_INSTRUMENTS_ROW_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_INSTRUMENTS_ROW_PARSER_H_

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/common/virtual_memory_mapping.h"
#include "src/trace_processor/importers/instruments/row.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::instruments_importer {

class RowDataTracker;

class RowParser : public InstrumentsRowParser {
 public:
  explicit RowParser(TraceProcessorContext*);
  ~RowParser() override = default;

  void ParseInstrumentsRow(int64_t, instruments_importer::Row) override;

 private:
  DummyMemoryMapping* GetDummyMapping(UniquePid upid);

  TraceProcessorContext* context_;
  RowDataTracker& data_;

  // Cache FrameId and binary mappings by instruments frame and binary
  // pointers, respectively. These are already de-duplicated in the
  // instruments XML parsing.
  base::FlatHashMap<BacktraceFrameId, FrameId> frame_to_frame_id_;
  base::FlatHashMap<BinaryId, VirtualMemoryMapping*> binary_to_mapping_;
  base::FlatHashMap<UniquePid, DummyMemoryMapping*> dummy_mappings_;

  // Cache callsites by FrameId in a prefix tree, where children in the
  // prefix tree are child frames at the callsite. This should be more
  // efficient than looking up frame+parent pairs in a hashmap.
  // TODO(leszeks): Verify that this is more efficient and share the code
  // with other importers.
  struct CallsiteTreeNode {
    CallsiteId callsite_id{0};
    base::FlatHashMap<FrameId, CallsiteTreeNode> next_frames{};
  };
  base::FlatHashMap<FrameId, CallsiteTreeNode> top_frames;
};

}  // namespace perfetto::trace_processor::instruments_importer

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_INSTRUMENTS_ROW_PARSER_H_
