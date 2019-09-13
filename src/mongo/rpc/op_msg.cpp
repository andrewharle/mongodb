
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/rpc/op_msg.h"

#include <bitset>
#include <set>

#include "mongo/base/data_type_endian.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

auto kAllSupportedFlags = OpMsg::kChecksumPresent | OpMsg::kMoreToCome;

bool containsUnknownRequiredFlags(uint32_t flags) {
    const uint32_t kRequiredFlagMask = 0xffff;  // Low 2 bytes are required, high 2 are optional.
    return (flags & ~kAllSupportedFlags & kRequiredFlagMask) != 0;
}

enum class Section : uint8_t {
    kBody = 0,
    kDocSequence = 1,
};

}  // namespace

uint32_t OpMsg::flags(const Message& message) {
    if (message.operation() != dbMsg)
        return 0;  // Other command protocols are the same as no flags set.

    return BufReader(message.singleData().data(), message.dataSize())
        .read<LittleEndian<uint32_t>>();
}

void OpMsg::replaceFlags(Message* message, uint32_t flags) {
    invariant(!message->empty());
    invariant(message->operation() == dbMsg);
    invariant(message->dataSize() >= static_cast<int>(sizeof(uint32_t)));

    DataView(message->singleData().data()).write<LittleEndian<uint32_t>>(flags);
}

OpMsg OpMsg::parse(const Message& message) try {
    // It is the caller's responsibility to call the correct parser for a given message type.
    invariant(!message.empty());
    invariant(message.operation() == dbMsg);

    const uint32_t flags = OpMsg::flags(message);
    uassert(ErrorCodes::IllegalOpMsgFlag,
            str::stream() << "Message contains illegal flags value: Ob"
                          << std::bitset<32>(flags).to_string(),
            !containsUnknownRequiredFlags(flags));

    constexpr int kCrc32Size = 4;
    const bool haveChecksum = flags & kChecksumPresent;
    const int checksumSize = haveChecksum ? kCrc32Size : 0;

    // The sections begin after the flags and before the checksum (if present).
    BufReader sectionsBuf(message.singleData().data() + sizeof(flags),
                          message.dataSize() - sizeof(flags) - checksumSize);

    // TODO some validation may make more sense in the IDL parser. I've tagged them with comments.
    bool haveBody = false;
    OpMsg msg;
    while (!sectionsBuf.atEof()) {
        const auto sectionKind = sectionsBuf.read<Section>();
        switch (sectionKind) {
            case Section::kBody: {
                uassert(40430, "Multiple body sections in message", !haveBody);
                haveBody = true;
                msg.body = sectionsBuf.read<Validated<BSONObj>>();
                break;
            }

            case Section::kDocSequence: {
                // We use an O(N^2) algorithm here and an O(N*M) algorithm below. These are fastest
                // for the current small values of N, but would be problematic if it is large.
                // If we need more document sequences, raise the limit and use a better algorithm.
                uassert(ErrorCodes::TooManyDocumentSequences,
                        "Too many document sequences in OP_MSG",
                        msg.sequences.size() < 2);  // Limit is <=2 since we are about to add one.

                // The first 4 bytes are the total size, including themselves.
                const auto remainingSize =
                    sectionsBuf.read<LittleEndian<int32_t>>() - sizeof(int32_t);
                BufReader seqBuf(sectionsBuf.skip(remainingSize), remainingSize);
                const auto name = seqBuf.readCStr();
                uassert(40431,
                        str::stream() << "Duplicate document sequence: " << name,
                        !msg.getSequence(name));  // TODO IDL

                msg.sequences.push_back({name.toString()});
                while (!seqBuf.atEof()) {
                    msg.sequences.back().objs.push_back(seqBuf.read<Validated<BSONObj>>());
                }
                break;
            }

            default:
                // Using uint32_t so we append as a decimal number rather than as a char.
                uasserted(40432, str::stream() << "Unknown section kind " << uint32_t(sectionKind));
        }
    }

    uassert(40587, "OP_MSG messages must have a body", haveBody);

    // Detect duplicates between doc sequences and body. TODO IDL
    // Technically this is O(N*M) but N is at most 2.
    for (const auto& docSeq : msg.sequences) {
        const char* name = docSeq.name.c_str();  // Pointer is redirected by next call.
        auto inBody =
            !dotted_path_support::extractElementAtPathOrArrayAlongPath(msg.body, name).eoo();
        uassert(40433,
                str::stream() << "Duplicate field between body and document sequence "
                              << docSeq.name,
                !inBody);
    }

    return msg;
} catch (const DBException& ex) {
    LOG(1) << "invalid message: " << ex.code() << " " << redact(ex) << " -- "
           << redact(hexdump(message.singleData().view2ptr(), message.size()));
    throw;
}

Message OpMsg::serialize() const {
    OpMsgBuilder builder;
    for (auto&& seq : sequences) {
        auto docSeq = builder.beginDocSequence(seq.name);
        for (auto&& obj : seq.objs) {
            docSeq.append(obj);
        }
    }
    builder.beginBody().appendElements(body);
    return builder.finish();
}

void OpMsg::shareOwnershipWith(const ConstSharedBuffer& buffer) {
    if (!body.isOwned()) {
        body.shareOwnershipWith(buffer);
    }
    for (auto&& seq : sequences) {
        for (auto&& obj : seq.objs) {
            if (!obj.isOwned()) {
                obj.shareOwnershipWith(buffer);
            }
        }
    }
}

auto OpMsgBuilder::beginDocSequence(StringData name) -> DocSequenceBuilder {
    invariant(_state == kEmpty || _state == kDocSequence);
    invariant(!_openBuilder);
    _openBuilder = true;
    _state = kDocSequence;
    _buf.appendStruct(Section::kDocSequence);
    int sizeOffset = _buf.len();
    _buf.skip(sizeof(int32_t));  // section size.
    _buf.appendStr(name, true);
    return DocSequenceBuilder(this, &_buf, sizeOffset);
}

void OpMsgBuilder::finishDocumentStream(DocSequenceBuilder* docSequenceBuilder) {
    invariant(_state == kDocSequence);
    invariant(_openBuilder);
    _openBuilder = false;
    const int32_t size = _buf.len() - docSequenceBuilder->_sizeOffset;
    invariant(size > 0);
    DataView(_buf.buf()).write<LittleEndian<int32_t>>(size, docSequenceBuilder->_sizeOffset);
}

BSONObjBuilder OpMsgBuilder::beginBody() {
    invariant(_state == kEmpty || _state == kDocSequence);
    _state = kBody;
    _buf.appendStruct(Section::kBody);
    invariant(_bodyStart == 0);
    _bodyStart = _buf.len();  // Cannot be 0.
    return BSONObjBuilder(_buf);
}

BSONObjBuilder OpMsgBuilder::resumeBody() {
    invariant(_state == kBody);
    invariant(_bodyStart != 0);
    return BSONObjBuilder(BSONObjBuilder::ResumeBuildingTag(), _buf, _bodyStart);
}

AtomicBool OpMsgBuilder::disableDupeFieldCheck_forTest{false};

Message OpMsgBuilder::finish() {
    if (kDebugBuild && !disableDupeFieldCheck_forTest.load()) {
        std::set<StringData> seenFields;
        for (auto elem : resumeBody().asTempObj()) {
            if (!(seenFields.insert(elem.fieldNameStringData()).second)) {
                severe() << "OP_MSG with duplicate field '" << elem.fieldNameStringData()
                         << "' : " << redact(resumeBody().asTempObj());
                fassert(40474, false);
            }
        }
    }

    invariant(_state == kBody);
    invariant(_bodyStart);
    invariant(!_openBuilder);
    _state = kDone;

    const auto size = _buf.len();
    MSGHEADER::View header(_buf.buf());
    header.setMessageLength(size);
    // header.setRequestMsgId(...); // These are currently filled in by the networking layer.
    // header.setResponseToMsgId(...);
    header.setOpCode(dbMsg);
    return Message(_buf.release());
}

}  // namespace mongo
