//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_PGGATE_PG_ANALYZE_H_
#define YB_YQL_PGGATE_PG_ANALYZE_H_

#include <memory>
#include <utility>

#include "yb/master/master.pb.h"
#include "yb/util/status.h"
#include "yb/yql/pggate/pg_dml_write.h"
#include "yb/yql/pggate/pg_expr.h"

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------
// ANALYZE
//--------------------------------------------------------------------------------------------------

class PgAnalyze : public PgStatement {
 public:
  PgAnalyze(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id)
      : PgStatement(std::move(pg_session)), table_id_(table_id) {}

  StmtOp stmt_op() const override { return StmtOp::STMT_ANALYZE; }

  // Execute.
  CHECKED_STATUS Exec();

  Result<int32_t> GetNumRows();

 private:
  CHECKED_STATUS GetOkOrRespError();

  const PgObjectId table_id_;
  master::AnalyzeTableResponsePB resp_;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_ANALYZE_H_
