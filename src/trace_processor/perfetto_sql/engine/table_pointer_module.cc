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

#include "src/trace_processor/perfetto_sql/engine/table_pointer_module.h"

#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

int TablePointerModule::Connect(sqlite3* db,
                                void*,
                                int,
                                const char* const*,
                                sqlite3_vtab** vtab,
                                char**) {
  static constexpr char kSchema[] = R"(
    CREATE TABLE x(
      c0 ANY,
      c1 ANY,
      c2 ANY,
      c3 ANY,
      c4 ANY,
      c5 ANY,
      c6 ANY,
      c7 ANY,
      c8 ANY,
      c9 ANY,
      c10 ANY,
      c11 ANY,
      c12 ANY,
      c13 ANY,
      c14 ANY,
      c15 ANY,
      tab BLOB HIDDEN,
      row INTEGER HIDDEN,
      PRIMARY KEY(row)
    ) WITHOUT ROWID
  )";
  if (int ret = sqlite3_declare_vtab(db, kSchema); ret != SQLITE_OK) {
    return ret;
  }
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  *vtab = res.release();
  return SQLITE_OK;
}

int TablePointerModule::Disconnect(sqlite3_vtab* vtab) {
  delete GetVtab(vtab);
  return SQLITE_OK;
}

int TablePointerModule::BestIndex(sqlite3_vtab* tab, sqlite3_index_info* info) {
  std::array<bool, kBindableColumnCount> bound_cols{};
  uint32_t bound_cols_count = 0;
  bool seen_tab_eq = false;
  for (int i = 0; i < info->nConstraint; ++i) {
    auto& in = info->aConstraint[i];
    auto& out = info->aConstraintUsage[i];
    // Ignore any unusable constraints.
    if (!in.usable) {
      continue;
    }
    // Disallow row constraints.
    if (in.iColumn == kRowColumnIndex) {
      return sqlite::utils::SetError(tab, "Constraint on row not allowed");
    }
    // Bind constraints.
    if (in.op == kBindConstraint) {
      if (in.iColumn >= kBindableColumnCount) {
        return sqlite::utils::SetError(tab, "Invalid bound column");
      }
      bool& bound = bound_cols[static_cast<uint32_t>(in.iColumn)];
      if (bound) {
        return sqlite::utils::SetError(tab, "Duplicate bound column");
      }
      // TODO(lalitm): all of the values here should be constants.
      out.argvIndex = kBoundColumnArgvOffset + in.iColumn;
      out.omit = true;
      bound = true;
      bound_cols_count++;
      continue;
    }
    // Constraint on tab.
    if (in.iColumn == kTableColumnIndex) {
      if (in.op != SQLITE_INDEX_CONSTRAINT_EQ) {
        return sqlite::utils::SetError(
            tab, "tab only supports equality constraints");
      }
      out.argvIndex = kTableArgvIndex;
      out.omit = true;
      seen_tab_eq = true;
      continue;
    }
    // Any other constraints on the columns.
    // TODO(lalitm): implement support for passing these down.
  }
  if (!seen_tab_eq) {
    return sqlite::utils::SetError(tab, "table must be bound");
  }
  if (bound_cols_count == 0) {
    return sqlite::utils::SetError(tab, "At least one column must be bound");
  }
  for (uint32_t i = 0; i < bound_cols_count; ++i) {
    if (!bound_cols[i]) {
      return sqlite::utils::SetError(tab, "Bound columns are not dense");
    }
  }
  return SQLITE_OK;
}

int TablePointerModule::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cursor) {
  std::unique_ptr<Cursor> c = std::make_unique<Cursor>();
  *cursor = c.release();
  return SQLITE_OK;
}

int TablePointerModule::Close(sqlite3_vtab_cursor* cursor) {
  delete GetCursor(cursor);
  return SQLITE_OK;
}

int TablePointerModule::Filter(sqlite3_vtab_cursor* cur,
                               int,
                               const char*,
                               int argc,
                               sqlite3_value** argv) {
  auto* c = GetCursor(cur);
  if (argc == 0) {
    return sqlite::utils::SetError(c->pVtab, "tab parameter is not set");
  }
  c->table = static_cast<const Table*>(sqlite3_value_pointer(argv[0], "TABLE"));
  if (!c->table) {
    return sqlite::utils::SetError(c->pVtab, "tab parameter is NULL");
  }
  c->col_count = 0;
  for (int i = 1; i < argc; ++i) {
    if (sqlite3_value_type(argv[i]) != SQLITE_TEXT) {
      return sqlite::utils::SetError(c->pVtab, "Column name is not text");
    }

    std::string_view tok(
        reinterpret_cast<const char*>(sqlite3_value_text(argv[i])));
    auto it = std::find_if(
        c->table->columns().begin(), c->table->columns().end(),
        [&tok](const ColumnLegacy& col) { return col.name() == tok; });
    if (it == c->table->columns().end()) {
      return sqlite::utils::SetError(c->pVtab,
                                     "column does not exist in table");
    }
    c->bound_col_to_table_index[c->col_count++] =
        static_cast<uint32_t>(std::distance(c->table->columns().begin(), it));
  }
  c->iterator = c->table->IterateRows();
  return SQLITE_OK;
}

int TablePointerModule::Next(sqlite3_vtab_cursor* cur) {
  auto* c = GetCursor(cur);
  ++(*c->iterator);
  return SQLITE_OK;
}

int TablePointerModule::Eof(sqlite3_vtab_cursor* cur) {
  return !*GetCursor(cur)->iterator;
}

int TablePointerModule::Column(sqlite3_vtab_cursor* cur,
                               sqlite3_context* ctx,
                               int raw_n) {
  auto* c = GetCursor(cur);
  auto N = static_cast<uint32_t>(raw_n);
  if (PERFETTO_UNLIKELY(N >= c->col_count)) {
    return sqlite::utils::SetError(c->pVtab,
                                   "Asking for value of non bound column");
  }
  uint32_t table_index = c->bound_col_to_table_index[N];
  sqlite::utils::ReportSqlValue(ctx, c->iterator->Get(table_index));
  return SQLITE_OK;
}

int TablePointerModule::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

int TablePointerModule::FindFunction(sqlite3_vtab*,
                                     int,
                                     const char* name,
                                     FindFunctionFn** fn,
                                     void**) {
  if (base::CaseInsensitiveEqual(name, "__intrinsic_table_ptr_bind")) {
    *fn = [](sqlite3_context* ctx, int, sqlite3_value**) {
      return sqlite::result::Error(ctx, "Should not be called.");
    };
    return SQLITE_INDEX_CONSTRAINT_FUNCTION + 1;
  }
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
