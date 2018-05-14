//
//  DBTests.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#if !FL_EMBEDDED

#include "FleeceTests.hh"
#include "Fleece.hh"
#include "MutableDict.hh"
#include "DB.hh"
#include <functional>
#include <unistd.h>

using namespace std;
using namespace fleece;


class DBTests {
public:
    unique_ptr<DB> db;
    alloc_slice populatedFrom;
    vector<alloc_slice> names;

    static constexpr const char* kDBPath = "/tmp/DB_test.fleecedb";
    static constexpr const char* kAltDBPath = "/tmp/DB_test_alt.fleecedb";

    DBTests() {
        unlink(kDBPath);
        reopen(DB::kCreateAndWrite);
    }

    void reopen(DB::OpenMode mode =DB::kWrite) {
        db.reset( new DB(kDBPath, mode) );
    }

    void populate() {
        populatedFrom = readFile(kTestFilesDir "1000people.fleece");
        auto people = Value::fromTrustedData(populatedFrom)->asArray();

        for (Array::iterator i(people); i; ++i) {
            auto person = i.value()->asDict();
            auto key = person->get("guid"_sl)->asString();
            names.emplace_back(key);
            db->put(key, DB::Insert, person);
        }
        db->commitChanges();
    }

    void iterateAndCheck() {
        set<alloc_slice> keys;
        for (DB::iterator i(db.get()); i; ++i) {
            CHECK(keys.insert(alloc_slice(i.key())).second == true);

            REQUIRE(i.value());
            auto guid = i.value()->get("guid"_sl);
            REQUIRE(guid);
            CHECK(guid->asString() == i.key());
        }
        CHECK(keys == set<alloc_slice>(names.begin(), names.end()));
    }

    void update(bool verbose =false) {
        reopen();
        auto dbSize = db->checkpoint();
        if (verbose)
            cerr << "Database is " << dbSize << " bytes\n";

        db->remove(names[123]);
        names.erase(names.begin() + 123);

        MutableDict *eleven = db->getMutable(names[11]);
        REQUIRE(eleven);
        if (verbose)
            cerr << "Eleven was: " << eleven->toJSONString() << "\n";
        eleven->set("name"_sl, "Eleven"_sl);
        eleven->set("age"_sl, 12);
        eleven->set("about"_sl, "REDACTED"_sl);
        if (verbose)
            cerr << "\nEleven is now: " << eleven->toJSONString() << "\n\n";
        db->commitChanges();
    }

    void modifyFile(function<void(FILE*)> callback) {
        db.reset(); // close DB

        FILE *f = fopen(kDBPath, "r+");
        REQUIRE(f);
        callback(f);
        fclose(f);
    }
};


TEST_CASE_METHOD(DBTests, "Create DB", "[DB]") {
    populate();

    db.reset( new DB(kDBPath) );

    for (auto name : names) {
        auto value = db->get(name);
        REQUIRE(value);
        REQUIRE(value->asDict());
        auto guid = value->asDict()->get("guid"_sl);
        REQUIRE(guid);
        CHECK(guid->asString() == name);
    }
}


TEST_CASE_METHOD(DBTests, "Iterate DB", "[DB]") {
    populate();
    iterateAndCheck();
}


TEST_CASE_METHOD(DBTests, "Small Update DB", "[DB]") {
    populate();
    auto checkpoint1 = db->checkpoint();
    update(true);
    iterateAndCheck();

    auto checkpoint2 = db->checkpoint();
    CHECK(checkpoint2 > checkpoint1);
    CHECK(db->previousCheckpoint() == checkpoint1);

    DB olderdb(*db, db->previousCheckpoint());
    CHECK(olderdb.checkpoint() == checkpoint1);
    CHECK(olderdb.previousCheckpoint() == 0);
    const Dict *eleven = olderdb.get(names[11]);
    REQUIRE(eleven);
    CHECK(eleven->get("name"_sl)->asString() == "Dollie Reyes"_sl);
}


TEST_CASE_METHOD(DBTests, "Export DB to new file", "[DB]") {
    populate();
    cerr << "Original database is " << db->checkpoint() << " bytes\n";
    update();
    cerr << "Updated database is " << db->checkpoint() << " bytes\n";
    db->writeTo(kAltDBPath);
    db.reset();
    db.reset( new DB(kAltDBPath, DB::kReadOnly) );
    cerr << "Exported database is " << db->checkpoint() << " bytes\n";
    iterateAndCheck();
}


TEST_CASE_METHOD(DBTests, "Corrupt DB header", "[DB]") {
    populate();
    update(false);

    modifyFile([](FILE *f) {
        fseeko(f, 0, SEEK_SET);
        fputc(0xFF, f);
    });

    CHECK_THROWS_AS(reopen(), FleeceException);
}

TEST_CASE_METHOD(DBTests, "Corrupt DB all trailers", "[DB]") {
    populate();

    modifyFile([](FILE *f) {
        fseeko(f, -1, SEEK_END);
        fputc(0xFF, f);
    });

    CHECK_THROWS_AS(reopen(), FleeceException);
}

TEST_CASE_METHOD(DBTests, "Corrupt DB by appending", "[DB]") {
    populate();
    update(false);
    CHECK(db->checkpoint() == 0x10e000);

    modifyFile([](FILE *f) {
        fseeko(f, 0, SEEK_END);
        fputs("O HAI! IM IN UR DATABASE, APPENDIN UR DATAZ", f);
    });

    reopen();
    CHECK(db->isDamaged());
    CHECK(db->checkpoint() == 0x10e000);

    MutableDict *eleven = db->getMutable(names[11]);
    REQUIRE(eleven);
    CHECK(eleven->get("name"_sl)->asString() == "Eleven"_sl);
}


TEST_CASE_METHOD(DBTests, "Corrupt DB by overwriting trailer", "[DB]") {
    populate();
    auto checkpoint1 = db->checkpoint();
    update(false);
    auto checkpoint2 = db->checkpoint();
    CHECK(checkpoint2 > checkpoint1);

    modifyFile([](FILE *f) {
        fseeko(f, -1, SEEK_END);
        fputc(0xFF, f);
    });

    // Verify file reopens to previous (first) checkpoint:
    reopen();
    CHECK(db->isDamaged());
    CHECK(db->checkpoint() == checkpoint1);

    // The changes should be gone since that checkpoint was damaged:
    MutableDict *eleven = db->getMutable(names[11]);
    REQUIRE(eleven);
    CHECK(eleven->get("name"_sl)->asString() == "Dollie Reyes"_sl);
}

#endif // !FL_EMBEDDED
