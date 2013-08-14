// Copyright (c) 2013, Cloudera, inc.

#include <boost/assign/list_of.hpp>
#include <gtest/gtest.h>
#include "common/row.h"
#include "common/rowblock.h"
#include "common/schema.h"
#include "common/wire_protocol.h"
#include "util/status.h"
#include "util/stopwatch.h"
#include "util/test_macros.h"
#include "util/test_util.h"

namespace kudu {

class WireProtocolTest : public KuduTest {
 public:
  WireProtocolTest()
    : schema_(boost::assign::list_of
              (ColumnSchema("col1", STRING))
              (ColumnSchema("col2", STRING))
              (ColumnSchema("col3", UINT32, true /* nullable */)),
              1) {
  }

  void FillRowBlockWithTestRows(RowBlock* block) {
    block->selection_vector()->SetAllTrue();

    for (int i = 0; i < block->nrows(); i++) {
      RowBlockRow row = block->row(i);
      *reinterpret_cast<Slice*>(row.mutable_cell_ptr(schema_, 0)) = Slice("hello world col1");
      *reinterpret_cast<Slice*>(row.mutable_cell_ptr(schema_, 1)) = Slice("hello world col2");
      *reinterpret_cast<uint32_t*>(row.mutable_cell_ptr(schema_, 2)) = 12345;
      row.cell(2).set_null(false);
    }
  }
 protected:
  Schema schema_;
};

TEST_F(WireProtocolTest, TestOKStatus) {
  Status s = Status::OK();
  AppStatusPB pb;
  StatusToPB(s, &pb);
  EXPECT_EQ(AppStatusPB::OK, pb.code());
  EXPECT_FALSE(pb.has_message());
  EXPECT_FALSE(pb.has_posix_code());

  Status s2 = StatusFromPB(pb);
  ASSERT_STATUS_OK(s2);
}

TEST_F(WireProtocolTest, TestBadStatus) {
  Status s = Status::NotFound("foo", "bar");
  AppStatusPB pb;
  StatusToPB(s, &pb);
  EXPECT_EQ(AppStatusPB::NOT_FOUND, pb.code());
  EXPECT_TRUE(pb.has_message());
  EXPECT_EQ("foo: bar", pb.message());
  EXPECT_FALSE(pb.has_posix_code());

  Status s2 = StatusFromPB(pb);
  EXPECT_TRUE(s2.IsNotFound());
  EXPECT_EQ(s.ToString(), s2.ToString());
}

TEST_F(WireProtocolTest, TestBadStatusWithPosixCode) {
  Status s = Status::NotFound("foo", "bar", 1234);
  AppStatusPB pb;
  StatusToPB(s, &pb);
  EXPECT_EQ(AppStatusPB::NOT_FOUND, pb.code());
  EXPECT_TRUE(pb.has_message());
  EXPECT_EQ("foo: bar", pb.message());
  EXPECT_TRUE(pb.has_posix_code());
  EXPECT_EQ(1234, pb.posix_code());

  Status s2 = StatusFromPB(pb);
  EXPECT_TRUE(s2.IsNotFound());
  EXPECT_EQ(1234, s2.posix_code());
  EXPECT_EQ(s.ToString(), s2.ToString());
}

TEST_F(WireProtocolTest, TestSchemaRoundTrip) {
  google::protobuf::RepeatedPtrField<ColumnSchemaPB> pbs;

  ASSERT_STATUS_OK(SchemaToColumnPBs(schema_, &pbs));
  ASSERT_EQ(3, pbs.size());

  // Column 0.
  EXPECT_TRUE(pbs.Get(0).is_key());
  EXPECT_EQ("col1", pbs.Get(0).name());
  EXPECT_EQ(STRING, pbs.Get(0).type());
  EXPECT_FALSE(pbs.Get(0).is_nullable());

  // Column 1.
  EXPECT_FALSE(pbs.Get(1).is_key());
  EXPECT_EQ("col2", pbs.Get(1).name());
  EXPECT_EQ(STRING, pbs.Get(1).type());
  EXPECT_FALSE(pbs.Get(1).is_nullable());

  // Column 2.
  EXPECT_FALSE(pbs.Get(2).is_key());
  EXPECT_EQ("col3", pbs.Get(2).name());
  EXPECT_EQ(UINT32, pbs.Get(2).type());
  EXPECT_TRUE(pbs.Get(2).is_nullable());

  // Convert back to a Schema object and verify they're identical.
  Schema schema2;
  ASSERT_STATUS_OK(ColumnPBsToSchema(pbs, &schema2));
  EXPECT_EQ(schema_.ToString(), schema2.ToString());
  EXPECT_EQ(schema_.num_key_columns(), schema2.num_key_columns());
}

// Test that, when non-contiguous key columns are passed, an error Status
// is returned.
TEST_F(WireProtocolTest, TestBadSchema_NonContiguousKey) {
  google::protobuf::RepeatedPtrField<ColumnSchemaPB> pbs;

  // Column 0: key
  ColumnSchemaPB* col_pb = pbs.Add();
  col_pb->set_name("c0");
  col_pb->set_type(STRING);
  col_pb->set_is_key(true);

  // Column 1: not a key
  col_pb = pbs.Add();
  col_pb->set_name("c1");
  col_pb->set_type(STRING);
  col_pb->set_is_key(false);

  // Column 2: marked as key. This is an error.
  col_pb = pbs.Add();
  col_pb->set_name("c2");
  col_pb->set_type(STRING);
  col_pb->set_is_key(true);

  Schema schema;
  Status s = ColumnPBsToSchema(pbs, &schema);
  ASSERT_STR_CONTAINS(s.ToString(), "Got out-of-order key column");
}

// Test that, when multiple columns with the same name are passed, an
// error Status is returned.
TEST_F(WireProtocolTest, TestBadSchema_DuplicateColumnName) {
  google::protobuf::RepeatedPtrField<ColumnSchemaPB> pbs;

  // Column 0:
  ColumnSchemaPB* col_pb = pbs.Add();
  col_pb->set_name("c0");
  col_pb->set_type(STRING);
  col_pb->set_is_key(true);

  // Column 1:
  col_pb = pbs.Add();
  col_pb->set_name("c1");
  col_pb->set_type(STRING);
  col_pb->set_is_key(false);

  // Column 2: same name as column 0
  col_pb = pbs.Add();
  col_pb->set_name("c0");
  col_pb->set_type(STRING);
  col_pb->set_is_key(false);

  Schema schema;
  Status s = ColumnPBsToSchema(pbs, &schema);
  ASSERT_STR_CONTAINS(s.ToString(), "Duplicate name present");
}

// Create a block of rows in protobuf form, then ensure that they
// can be read back out.
TEST_F(WireProtocolTest, TestRowBlockRoundTrip) {
  const int kNumRows = 10;

  RowwiseRowBlockPB pb;

  // Build a set of rows into the protobuf.
  RowBuilder rb(schema_);
  for (int i = 0; i < kNumRows; i++) {
    rb.Reset();
    rb.AddString(StringPrintf("col1 %d", i));
    rb.AddString(StringPrintf("col2 %d", i));
    if (i % 2 == 1) {
      rb.AddNull();
    } else {
      rb.AddUint32(i);
    }
    AddRowToRowBlockPB(rb.row(), &pb);
  }

  // Extract the rows back out, verify that the results are the same
  // as the input.
  vector<const uint8_t*> row_ptrs;
  ASSERT_STATUS_OK(ExtractRowsFromRowBlockPB(schema_, &pb, &row_ptrs));
  ASSERT_EQ(kNumRows, row_ptrs.size());
  for (int i = 0; i < kNumRows; i++) {
    ConstContiguousRow row(schema_, row_ptrs[i]);
    ASSERT_EQ(StringPrintf("col1 %d", i),
              schema_.ExtractColumnFromRow<STRING>(row, 0)->ToString());
    ASSERT_EQ(StringPrintf("col2 %d", i),
              schema_.ExtractColumnFromRow<STRING>(row, 1)->ToString());
    if (i % 2 == 1) {
      ASSERT_TRUE(row.is_null(schema_, 2));
    } else {
      ASSERT_EQ(i, *schema_.ExtractColumnFromRow<UINT32>(row, 2));
    }
  }
}

// Create a block of rows in columnar layout and ensure that it can be
// converted to and from protobuf.
TEST_F(WireProtocolTest, TestColumnarRowBlockToPB) {
  // Set up a row block with a single row in it.
  Arena arena(1024, 1024 * 1024);
  RowBlock block(schema_, 1, &arena);
  FillRowBlockWithTestRows(&block);

  // Convert to PB.
  RowwiseRowBlockPB pb;
  ConvertRowBlockToPB(block, &pb);
  SCOPED_TRACE(pb.DebugString());

  // Convert back to a row, ensure that the resulting row is the same
  // as the one we put in.
  vector<const uint8_t*> row_ptrs;
  ASSERT_STATUS_OK(ExtractRowsFromRowBlockPB(schema_, &pb, &row_ptrs));
  ASSERT_EQ(1, row_ptrs.size());
  ConstContiguousRow row_roundtripped(schema_, row_ptrs[0]);
  ASSERT_EQ(schema_.DebugRow(block.row(0)), schema_.DebugRow(row_roundtripped));
}

#ifdef NDEBUG
TEST_F(WireProtocolTest, TestColumnarRowBlockToPBBenchmark) {
  Arena arena(1024, 1024 * 1024);
  RowBlock block(schema_, 100000, &arena);
  FillRowBlockWithTestRows(&block);

  RowwiseRowBlockPB pb;

  const int kNumTrials = AllowSlowTests() ? 100 : 10;
  LOG_TIMING(INFO, "Converting to PB") {
    for (int i = 0; i < kNumTrials; i++) {
      pb.Clear();
      ConvertRowBlockToPB(block, &pb);
    }
  }
}
#endif

// Test that trying to extract rows from an invalid block correctly returns
// Corruption statuses.
TEST_F(WireProtocolTest, TestInvalidRowBlock) {
  Schema schema(boost::assign::list_of(ColumnSchema("col1", STRING)),
                1);
  RowwiseRowBlockPB pb;
  vector<const uint8_t*> row_ptrs;

  // Too short to be valid data.
  pb.mutable_rows()->assign("x");
  pb.set_num_rows(1);
  Status s = ExtractRowsFromRowBlockPB(schema, &pb, &row_ptrs);
  ASSERT_STR_CONTAINS(s.ToString(), "Corruption: Row block has 1 bytes of data");

  // Bad pointer into indirect data.
  pb.mutable_rows()->assign("xxxxxxxxxxxxxxxx");
  pb.set_num_rows(1);
  s = ExtractRowsFromRowBlockPB(schema, &pb, &row_ptrs);
  ASSERT_STR_CONTAINS(s.ToString(),
                      "Corruption: Row #0 contained bad indirect slice");
}

// Test serializing a block which has a selection vector but no columns.
// This is the sort of result that is returned from a scan with an empty
// projection (a COUNT(*) query).
TEST_F(WireProtocolTest, TestBlockWithNoColumns) {
  Schema empty(std::vector<ColumnSchema>(), 0);
  Arena arena(1024, 1024 * 1024);
  RowBlock block(empty, 1000, &arena);
  block.selection_vector()->SetAllTrue();
  // Unselect 100 rows
  for (int i = 0; i < 100; i++) {
    block.selection_vector()->SetRowUnselected(i * 2);
  }
  ASSERT_EQ(900, block.selection_vector()->CountSelected());

  // Convert it to protobuf, ensure that the results look right.
  RowwiseRowBlockPB pb;
  ConvertRowBlockToPB(block, &pb);
  ASSERT_EQ(900, pb.num_rows());
}

} // namespace kudu
