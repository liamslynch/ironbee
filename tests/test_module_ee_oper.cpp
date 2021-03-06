//////////////////////////////////////////////////////////////////////////////
// Licensed to Qualys, Inc. (QUALYS) under one or more
// contributor license agreements.  See the NOTICE file distributed with
// this work for additional information regarding copyright ownership.
// QUALYS licenses this file to You under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief IronBee --- Eudoxus operator module tests
///
/// @author Craig Forbes <cforbes@qualys.com>
//////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "base_fixture.h"
#include <ironbee/operator.h>

// @todo Remove once ib_engine_operator_get() is available.
#include "engine_private.h"

class EeOperModuleTest : public BaseModuleFixture
{
public:
    EeOperModuleTest() : BaseModuleFixture("ibmod_ee.so")
    {
    }

    void SetUp()
    {
        BaseModuleFixture::SetUp();
        performTx();
    }
    void configureIronBee(void)
    {
        BaseModuleFixture::configureIronBee("EeOperModuleTest.config");
    }
    void generateRequestHeader( )
    {
        addRequestHeader("Host", "UnitTest");
        addRequestHeader("X-MyHeader", "header1");
        addRequestHeader("X-MyHeader", "string_to_match");
    }
    void generateResponseHeader( )
    {
        addResponseHeader("Content-Type", "text/html");
        addResponseHeader("X-MyHeader", "header2");
        addResponseHeader("X-MyHeader", "puke");
    }
};

TEST_F(EeOperModuleTest, test_load_module)
{
    ib_operator_t op;
    // Ensure that the operator exists.
    ASSERT_EQ(IB_OK, ib_hash_get(ib_engine->operators,
                                 reinterpret_cast<void**>(&op),
                                 "ee_match_any"));
}

TEST_F(EeOperModuleTest, test_ee_match_any_success)
{
    ib_operator_t op;
    ib_field_t *f;
    ib_num_t n;

    // Ensure that the operator exists.
    ASSERT_EQ(IB_OK, ib_hash_get(ib_engine->operators,
                                 (void**)&op,
                                 "ee_match_any"));

    ASSERT_EQ(IB_OK, ib_data_get(ib_tx->data, "request_matched", &f));
    ASSERT_EQ(IB_FTYPE_NUM, f->type);
    ib_field_value(f, ib_ftype_num_out(&n));
    EXPECT_EQ(1, n);

    // check that the capture has the text matched
    ib_tx = ib_conn->tx;
    ib_field_t *ib_field;
    const ib_list_t *ib_list;
    const ib_bytestr_t *bs;
    ASSERT_EQ(IB_OK, ib_data_get(ib_tx->data, IB_TX_CAPTURE":0", &ib_field));
    ASSERT_TRUE(ib_field);
    ASSERT_EQ(static_cast<ib_ftype_t>(IB_FTYPE_LIST), ib_field->type);
    ib_field_value(ib_field, ib_ftype_list_out(&ib_list));
    ASSERT_EQ(1U, IB_LIST_ELEMENTS(ib_list));
    ib_field = (ib_field_t *)IB_LIST_NODE_DATA(IB_LIST_LAST(ib_list));
    ASSERT_TRUE(ib_field);
    ASSERT_EQ(static_cast<ib_ftype_t>(IB_FTYPE_BYTESTR), ib_field->type);
    ASSERT_EQ(IB_OK, ib_field_value(ib_field, ib_ftype_bytestr_out(&bs)));
    ASSERT_EQ(15UL, ib_bytestr_length(bs));
    EXPECT_EQ(0, strncmp("string_to_match", (const char *)ib_bytestr_const_ptr(bs), 15));
}

TEST_F(EeOperModuleTest, test_ee_match_any_fail)
{
    ib_operator_t op;
    ib_field_t *f;
    ib_num_t n;

    // Ensure that the operator exists.
    ASSERT_EQ(IB_OK, ib_hash_get(ib_engine->operators,
                                 (void**)&op,
                                 "ee_match_any"));

    ASSERT_EQ(IB_OK, ib_data_get(ib_tx->data, "response_matched", &f));
    ASSERT_EQ(IB_FTYPE_NUM, f->type);
    ib_field_value(f, ib_ftype_num_out(&n));
    EXPECT_EQ(0, n);
}
