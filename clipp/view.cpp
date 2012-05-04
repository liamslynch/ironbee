/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee &mdash; CLIPP View Consumer Implementation
 *
 * @author Christopher Alfeld <calfeld@qualys.com>
 */

#include "view.hpp"

#include <boost/foreach.hpp>
#include <boost/function.hpp>
#include <boost/format.hpp>

using namespace std;

namespace IronBee {
namespace CLIPP {

struct ViewConsumer::State
{
    boost::function<void(const Input::input_p&)> viewer;
};

namespace {

bool is_not_printable(char c)
{
    return (c < 32 || c > 126) && (c != 10) && (c != 13);
}

void output_with_escapes(const char* b, const char* e)
{
    const char* i = b;
    while (i < e) {
        const char* j = find_if(i, e, &is_not_printable);
        if (j == e) {
            cout.write(i, e - i);
            i = e;
        }
        else {
            if (j > i) {
                cout.write(i, j - i);
                i = j;
            }
            cout << "[" << static_cast<uint32_t>(*i) << "]";
            ++i;
        }
    }
}

using namespace Input;

struct ViewDelegate :
    public Delegate
{
    //! Output ConnectionEvent.
    static
    void connection_event(const ConnectionEvent& event)
    {
        cout << event.local_ip << ":" << event.local_port
             << " <--> "
             << event.remote_ip << ":" << event.remote_port
             ;
    }

    //! Output DataEvent.
    static
    void data_event(const DataEvent& event)
    {
        output_with_escapes(
            event.data.data,
            event.data.data + event.data.length
        );
    }

    //! Output HeaderEven& eventt
    static
    void headers_event(const HeaderEvent& event)
    {
        BOOST_FOREACH(const header_t& header, event.headers) {
            cout << header.first << ": " << header.second << endl;
        }
    }

    //! CONNECTION_OPENED
    void connection_opened(const ConnectionEvent& event)
    {
        cout << "=== CONNECTION_OPENED: ";
        connection_event(event);
        cout << " ===" << endl;
    }

    //! CONNECTION_CLOSED
    void connection_closed(const NullEvent& event)
    {
        cout << "=== CONNECTION_CLOSED ===" << endl;
    }

    //! CONNECTION_DATA_IN
    void connection_data_in(const DataEvent& event)
    {
        cout << "=== CONNECTION_DATA_IN ===" << endl;
        data_event(event);
    }

    //! CONNECTION_DATA_OUT
    void connection_data_out(const DataEvent& event)
    {
        cout << "=== CONNECTION_DATA_OUT ===" << endl;
        data_event(event);
    }

    //! REQUEST_STARTED
    void request_started(const RequestEvent& event)
    {
        cout << "=== REQUEST_STARTED: "
             << event.method << " " << event.uri << " " << event.uri
             << " ===" << endl;
        if (event.raw.data) {
            cout << event.raw << endl;
        }
    }

    //! REQUEST_HEADERS
    void request_headers(const HeaderEvent& event) {
        cout << "=== REQUEST_HEADERS ===" << endl;
        headers_event(event);
    }

    //! REQUEST_BODY
    void request_body(const DataEvent& event)
    {
        cout << "=== REQUEST_BODY ===" << endl;
        data_event(event);
    }

    //! REQUEST_FINISHED
    void request_finished(const NullEvent& event)
    {
        cout << "=== REQUEST_FINISHED ===" << endl;
    }

    //! RESPONSE_STARTED
    void response_started(const ResponseEvent& event)
    {
        cout << "=== RESPONSE_STARTED "
             << event.protocol << " " << event.status << " " << event.message
             << " ===" << endl;
        if (event.raw.data) {
            cout << event.raw << endl;
        }
    }

    //! RESPONSE_HEADERS
    void response_headers(const HeaderEvent& event)
    {
        cout << "=== RESPONSE HEADERS ===" << endl;
        headers_event(event);
    }

    //! RESPONSE_BODY
    void response_body(const DataEvent& event)
    {
        cout << "=== RESPONSE BODY ===" << endl;
        data_event(event);
    }

    //! RESPONSE_FINISHED
    void response_finished(const NullEvent& event)
    {
        cout << "=== RESPONSE FINISHED ===" << endl;
    }
};

void view_full(const input_p& input)
{
    if (input->id.empty()) {
        cout << "---- No ID Provided ----" << endl;
    }
    else {
        cout << "---- " << input->id << " ----" << endl;
    }
    ViewDelegate viewer;
    input->connection.dispatch(viewer);
}

void view_id(const input_p& input)
{
    if (input->id.empty()) {
        cout << "---- No ID Provided ----" << endl;
    }
    else {
        cout << "---- " << input->id << " ----" << endl;
    }
}

void view_summary(const input_p& input)
{
    string id("NO ID");

    if (! input->id.empty()) {
        id = input->id;
    }

    size_t num_txs = input->connection.transactions.size();

    if (input->connection.pre_transaction_events.empty()) {
        // no IP information.
        cout << boost::format("%36s NO CONNECTION INFO %5d") % id % num_txs
             << endl;
    }
    const ConnectionEvent& connection_event =
        dynamic_cast<ConnectionEvent&>(
            *input->connection.pre_transaction_events.front()
        );
    cout << boost::format("%-40s %20s <-> %-20s %5d txs") % id %
        (boost::format("%s:%d") %
            connection_event.local_ip % connection_event.local_port
        ) %
        (boost::format("%s:%d") %
            connection_event.remote_ip % connection_event.remote_port
        ) %
        num_txs
         << endl;
}

}

ViewConsumer::ViewConsumer(const std::string& arg) :
    m_state(new State())
{
    if (arg == "id") {
        m_state->viewer = view_id;
    }
    else if (arg == "summary") {
        m_state->viewer = view_summary;
    }
    else if (arg.empty()) {
        m_state->viewer = view_full;
    }
    else {
        throw runtime_error("Unknown View argument: " + arg);
    }
}

bool ViewConsumer::operator()(const input_p& input)
{
    if ( ! input ) {
        return true;
    }

    m_state->viewer(input);
    return true;
}

ViewModifier::ViewModifier(const std::string& arg) :
    m_consumer(arg)
{
    // nop
}

bool ViewModifier::operator()(input_p& input)
{
    return m_consumer(input);
}

} // CLIPP
} // IronBee