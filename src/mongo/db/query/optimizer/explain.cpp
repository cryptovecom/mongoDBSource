/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/explain.h"

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/core/demangle.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/util/assert_util.h"


namespace mongo::optimizer {

namespace {
/**
 * If the input expression is a constant or a variable, or it is an EvalFilter/Path which has an
 * identity path and input which itself is constant or variable, then return a pointer to the deepst
 * simple expression.
 */
template <class T>
ABT::reference_type getTrivialExprPtr(const ABT& n) {
    if (n.is<Constant>() || n.is<Variable>()) {
        return n.ref();
    }
    if (const auto* ptr = n.cast<T>();
        ptr != nullptr && ptr->getPath().template is<PathIdentity>()) {
        return getTrivialExprPtr<T>(ptr->getInput());
    }
    return {};
}

}  // namespace

ABTPrinter::ABTPrinter(PlanAndProps planAndProps,
                       const ExplainVersion explainVersion,
                       QueryParameterMap qpMap)
    : _planAndProps(std::move(planAndProps)),
      _explainVersion(explainVersion),
      _queryParameters(std::move(qpMap)) {}

BSONObj ABTPrinter::explainBSON() const {
    const auto explainPlanStr = [&](const std::string& planStr) {
        BSONObjBuilder builder;
        builder.append("plan", planStr);
        return builder.done().getOwned();
    };

    switch (_explainVersion) {
        case ExplainVersion::V1:
            return explainPlanStr(ExplainGenerator::explain(_planAndProps._node));

        case ExplainVersion::V2:
            return explainPlanStr(ExplainGenerator::explainV2(_planAndProps._node));

        case ExplainVersion::V2Compact:
            return explainPlanStr(ExplainGenerator::explainV2Compact(_planAndProps._node));

        case ExplainVersion::V3:
            return ExplainGenerator::explainBSONObj(
                _planAndProps._node, true /*displayProperties*/, _planAndProps._map);

        case ExplainVersion::UserFacingExplain: {
            UserFacingExplain ex(_planAndProps._map);
            return ex.explain(_planAndProps._node);
        }
        case ExplainVersion::Vmax:
            // Should not be seeing this value here.
            break;
    }

    MONGO_UNREACHABLE;
}

BSONObj ABTPrinter::getQueryParameters() const {
    // To obtain consistent explain results, we display the parameters in the order of their sorted
    // ids.
    std::vector<int32_t> paramIds;
    for (const auto& elem : _queryParameters) {
        paramIds.push_back(elem.first);
    }
    std::sort(paramIds.begin(), paramIds.end());

    BSONObjBuilder result;
    for (const auto& paramId : paramIds) {
        std::stringstream idStream;
        idStream << paramId;
        BSONObjBuilder paramBuilder(result.subobjStart(idStream.str()));
        const auto& constant = _queryParameters.at(paramId).get();
        paramBuilder.append("value", sbe::value::print(constant));

        std::stringstream typeStream;
        typeStream << constant.first;
        paramBuilder.append("type", typeStream.str());

        paramBuilder.doneFast();
    }

    return result.obj();
}

bool constexpr operator<(const ExplainVersion v1, const ExplainVersion v2) {
    return static_cast<int>(v1) < static_cast<int>(v2);
}
bool constexpr operator<=(const ExplainVersion v1, const ExplainVersion v2) {
    return static_cast<int>(v1) <= static_cast<int>(v2);
}
bool constexpr operator>(const ExplainVersion v1, const ExplainVersion v2) {
    return static_cast<int>(v1) > static_cast<int>(v2);
}
bool constexpr operator>=(const ExplainVersion v1, const ExplainVersion v2) {
    return static_cast<int>(v1) >= static_cast<int>(v2);
}

static constexpr ExplainVersion kDefaultExplainVersion = ExplainVersion::V1;

enum class CommandType { Indent, Unindent, AddLine };

struct CommandStruct {
    CommandStruct() = default;
    CommandStruct(const CommandType type, std::string str) : _type(type), _str(std::move(str)) {}

    CommandType _type;
    std::string _str;
};

using CommandVector = std::vector<CommandStruct>;

/**
 * Helper class for building indented, multiline strings.
 *
 * The main operations it supports are:
 *   - Print a single value, of any type that supports '<<' to std::ostream.
 *   - Indent/unindent, and add newlines.
 *   - Print another ExplainPrinterImpl, preserving its 2D layout.
 *
 * Being able to print another whole printer makes it easy to build these 2D strings
 * bottom-up, without passing around a std::ostream. It also allows displaying
 * child elements in a different order than they were visited.
 */
template <const ExplainVersion version = kDefaultExplainVersion>
class ExplainPrinterImpl {
public:
    ExplainPrinterImpl()
        : _cmd(),
          _os(),
          _osDirty(false),
          _indentCount(0),
          _childrenRemaining(0),
          _inlineNextChild(false),
          _cmdInsertPos(-1) {}

    ~ExplainPrinterImpl() {
        uassert(6624003, "Unmatched indentations", _indentCount == 0);
        uassert(6624004, "Incorrect child count mark", _childrenRemaining == 0);
    }

    ExplainPrinterImpl(const ExplainPrinterImpl& other) = delete;
    ExplainPrinterImpl& operator=(const ExplainPrinterImpl& other) = delete;

    explicit ExplainPrinterImpl(const std::string& initialStr) : ExplainPrinterImpl() {
        print(initialStr);
    }

    ExplainPrinterImpl(ExplainPrinterImpl&& other) noexcept
        : _cmd(std::move(other._cmd)),
          _os(std::move(other._os)),
          _osDirty(other._osDirty),
          _indentCount(other._indentCount),
          _childrenRemaining(other._childrenRemaining),
          _inlineNextChild(other._inlineNextChild),
          _cmdInsertPos(other._cmdInsertPos) {}

    template <class T>
    ExplainPrinterImpl& print(const T& t) {
        _os << t;
        _osDirty = true;
        return *this;
    }

    ExplainPrinterImpl& print(StringData s) {
        print(s.empty() ? "<empty>" : s.rawData());
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& print(const StrongStringAlias<TagType>& t) {
        print(t.value().empty() ? "<empty>" : t.value());
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& print(const StrongDoubleAlias<TagType>& t) {
        print(t._value);
        return *this;
    }

    /**
     * Here and below: "other" printer(s) may be siphoned out.
     */
    ExplainPrinterImpl& print(ExplainPrinterImpl& other) {
        return print(other, false /*singleLevel*/);
    }

    template <class P>
    ExplainPrinterImpl& printSingleLevel(P& other, const std::string& singleLevelSpacer = " ") {
        return print(other, true /*singleLevel*/, singleLevelSpacer);
    }

    ExplainPrinterImpl& printAppend(ExplainPrinterImpl& other) {
        // Ignore append
        return print(other);
    }

    ExplainPrinterImpl& print(std::vector<ExplainPrinterImpl>& other) {
        for (auto&& element : other) {
            print(element);
        }
        return *this;
    }

    ExplainPrinterImpl& printAppend(std::vector<ExplainPrinterImpl>& other) {
        // Ignore append.
        return print(other);
    }

    ExplainPrinterImpl& setChildCount(const size_t childCount, const bool noInline = false) {
        if (version == ExplainVersion::V1) {
            return *this;
        }

        if (!noInline && version == ExplainVersion::V2Compact && childCount == 1) {
            _inlineNextChild = true;
            _childrenRemaining = childCount;
            return *this;
        }

        _childrenRemaining = childCount;
        indent("");
        for (int i = 0; i < _childrenRemaining - 1; i++) {
            indent("|");
        }
        return *this;
    }

    ExplainPrinterImpl& maybeReverse() {
        if (version > ExplainVersion::V1) {
            _cmdInsertPos = _cmd.size();
        }
        return *this;
    }

    ExplainPrinterImpl& fieldName(const std::string& name,
                                  const ExplainVersion minVersion = ExplainVersion::V1,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        if (minVersion <= version && maxVersion >= version) {
            print(name);
            print(": ");
        }
        return *this;
    }

    ExplainPrinterImpl& separator(const std::string& separator) {
        return print(separator);
    }

    std::string str() {
        newLine();

        std::ostringstream os;
        std::vector<std::string> linePrefix;

        for (const auto& cmd : _cmd) {
            switch (cmd._type) {
                case CommandType::Indent:
                    linePrefix.push_back(cmd._str);
                    break;

                case CommandType::Unindent: {
                    linePrefix.pop_back();
                    break;
                }

                case CommandType::AddLine: {
                    for (const std::string& element : linePrefix) {
                        if (!element.empty()) {
                            os << element << ((version == ExplainVersion::V1) ? " " : "   ");
                        }
                    }
                    os << cmd._str << "\n";
                    break;
                }

                default: {
                    MONGO_UNREACHABLE;
                }
            }
        }

        return os.str();
    }

    /**
     * Ends the current line, if there is one. Repeated calls do not create
     * blank lines.
     */
    void newLine() {
        if (!_osDirty) {
            return;
        }
        const std::string& str = _os.str();
        _cmd.emplace_back(CommandType::AddLine, str);
        _os.str("");
        _os.clear();
        _osDirty = false;
    }

    const CommandVector& getCommands() const {
        return _cmd;
    }

private:
    template <class P>
    ExplainPrinterImpl& print(P& other,
                              const bool singleLevel,
                              const std::string& singleLevelSpacer = " ") {
        CommandVector toAppend;
        if (_cmdInsertPos >= 0) {
            toAppend = CommandVector(_cmd.cbegin() + _cmdInsertPos, _cmd.cend());
            _cmd.resize(static_cast<size_t>(_cmdInsertPos));
        }

        const bool hadChildrenRemaining = _childrenRemaining > 0;
        if (hadChildrenRemaining) {
            _childrenRemaining--;
        }
        other.newLine();

        if (singleLevel) {
            uassert(6624071, "Unexpected dirty status", _osDirty);

            bool first = true;
            for (const auto& element : other.getCommands()) {
                if (element._type == CommandType::AddLine) {
                    if (first) {
                        first = false;
                    } else {
                        _os << singleLevelSpacer;
                    }
                    _os << element._str;
                }
            }
        } else if (_inlineNextChild) {
            _inlineNextChild = false;
            // Print 'other' without starting a new line.
            // Embed its first line into our current one, and keep the rest of its commands.
            bool first = true;
            for (const CommandStruct& element : other.getCommands()) {
                if (first && element._type == CommandType::AddLine) {
                    _os << singleLevelSpacer << element._str;
                } else {
                    newLine();
                    _cmd.push_back(element);
                }
                first = false;
            }
        } else {
            newLine();
            // If 'hadChildrenRemaining' then 'other' represents a child of 'this', which means
            // there was a prior call to setChildCount() that added indentation for it.
            // If '! hadChildrenRemaining' then create indentation for it now.
            if (!hadChildrenRemaining) {
                indent();
            }
            for (const auto& element : other.getCommands()) {
                _cmd.push_back(element);
            }
            unIndent();
        }

        if (_cmdInsertPos >= 0) {
            std::copy(toAppend.cbegin(), toAppend.cend(), std::back_inserter(_cmd));
        }

        return *this;
    }

    void indent(std::string s = " ") {
        newLine();
        _indentCount++;
        _cmd.emplace_back(CommandType::Indent, std::move(s));
    }

    void unIndent() {
        newLine();
        _indentCount--;
        _cmd.emplace_back(CommandType::Unindent, "");
    }

    // Holds completed lines, and indent/unIndent commands.
    // When '_cmdInsertPos' is nonnegative, some of these lines and commands belong
    // after the currently-being-built line.
    CommandVector _cmd;
    // Holds the incomplete line currently being built. Once complete this will become the last
    // line, unless '_cmdInsertPos' is nonnegative.
    std::ostringstream _os;
    // True means we have an incomplete line in '_os'.
    // Once the line is completed with newLine(), this flag is false until
    // we begin building a new one with print().
    bool _osDirty;
    int _indentCount;
    int _childrenRemaining;
    bool _inlineNextChild;
    // When nonnegative, indicates the insertion point where completed lines
    // should be added to '_cmd'. -1 means completed lines will be added at the end.
    int _cmdInsertPos;
};

template <>
class ExplainPrinterImpl<ExplainVersion::V3> {
    static constexpr ExplainVersion version = ExplainVersion::V3;

public:
    ExplainPrinterImpl() {
        reset();
    }

    ~ExplainPrinterImpl() {
        if (_initialized) {
            releaseValue(_tag, _val);
        }
    }

    ExplainPrinterImpl(const ExplainPrinterImpl& other) = delete;
    ExplainPrinterImpl& operator=(const ExplainPrinterImpl& other) = delete;

    ExplainPrinterImpl(ExplainPrinterImpl&& other) noexcept {
        _nextFieldName = std::move(other._nextFieldName);
        _initialized = other._initialized;
        _canAppend = other._canAppend;
        _tag = other._tag;
        _val = other._val;
        _fieldNameSet = std::move(other._fieldNameSet);

        other.reset();
    }

    explicit ExplainPrinterImpl(const std::string& nodeName) : ExplainPrinterImpl() {
        fieldName("nodeType").print(nodeName);
    }

    auto moveValue() {
        auto result = std::pair<sbe::value::TypeTags, sbe::value::Value>(_tag, _val);
        reset();
        return result;
    }

    ExplainPrinterImpl& print(const bool v) {
        addValue(sbe::value::TypeTags::Boolean, v);
        return *this;
    }

    ExplainPrinterImpl& print(const int64_t v) {
        addValue(sbe::value::TypeTags::NumberInt64, sbe::value::bitcastFrom<int64_t>(v));
        return *this;
    }

    ExplainPrinterImpl& print(const int32_t v) {
        addValue(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int32_t>(v));
        return *this;
    }

    ExplainPrinterImpl& print(const size_t v) {
        addValue(sbe::value::TypeTags::NumberInt64, sbe::value::bitcastFrom<size_t>(v));
        return *this;
    }

    ExplainPrinterImpl& print(const double v) {
        addValue(sbe::value::TypeTags::NumberDouble, sbe::value::bitcastFrom<double>(v));
        return *this;
    }

    ExplainPrinterImpl& print(const std::pair<sbe::value::TypeTags, sbe::value::Value> v) {
        if (sbe::value::tagToType(v.first) == BSONType::EOO &&
            v.first != sbe::value::TypeTags::Nothing) {
            if (v.first == sbe::value::TypeTags::makeObjSpec) {
                // We want to append a stringified version of MakeObjSpec to explain here.
                auto [mosTag, mosVal] =
                    sbe::value::makeNewString(sbe::value::getMakeObjSpecView(v.second)->toString());
                addValue(mosTag, mosVal);
            } else {
                // Extended types need to implement their own explain, since we can't directly
                // convert them to bson.
                MONGO_UNREACHABLE_TASSERT(7936708);
            }

        } else {
            auto [tag, val] = sbe::value::copyValue(v.first, v.second);
            addValue(tag, val);
        }

        return *this;
    }

    ExplainPrinterImpl& print(const std::string& s) {
        printStringInternal(s);
        return *this;
    }

    ExplainPrinterImpl& print(StringData s) {
        printStringInternal(s);
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& print(const StrongStringAlias<TagType>& s) {
        printStringInternal(s.value());
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& print(const StrongDoubleAlias<TagType>& v) {
        return print(v._value);
    }

    ExplainPrinterImpl& print(const char* s) {
        return print(static_cast<std::string>(s));
    }

    /**
     * Here and below: "other" printer(s) may be siphoned out.
     */
    ExplainPrinterImpl& print(ExplainPrinterImpl& other) {
        return print(other, false /*append*/);
    }

    ExplainPrinterImpl& printSingleLevel(ExplainPrinterImpl& other,
                                         const std::string& /*singleLevelSpacer*/ = " ") {
        // Ignore single level.
        return print(other);
    }

    ExplainPrinterImpl& printAppend(ExplainPrinterImpl& other) {
        return print(other, true /*append*/);
    }

    ExplainPrinterImpl& print(std::vector<ExplainPrinterImpl>& other) {
        return print(other, false /*append*/);
    }

    ExplainPrinterImpl& printAppend(std::vector<ExplainPrinterImpl>& other) {
        return print(other, true /*append*/);
    }

    ExplainPrinterImpl& setChildCount(const size_t /*childCount*/) {
        // Ignored.
        return *this;
    }

    ExplainPrinterImpl& maybeReverse() {
        // Ignored.
        return *this;
    }

    template <size_t N>
    ExplainPrinterImpl& fieldName(const char (&name)[N],
                                  const ExplainVersion minVersion = ExplainVersion::V1,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        fieldNameInternal(name, minVersion, maxVersion);
        return *this;
    }

    ExplainPrinterImpl& fieldName(const std::string& name,
                                  const ExplainVersion minVersion = ExplainVersion::V1,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        fieldNameInternal(name, minVersion, maxVersion);
        return *this;
    }

    template <class TagType>
    ExplainPrinterImpl& fieldName(const StrongStringAlias<TagType>& name,
                                  const ExplainVersion minVersion = ExplainVersion::V1,
                                  const ExplainVersion maxVersion = ExplainVersion::Vmax) {
        fieldNameInternal(name.value().toString(), minVersion, maxVersion);
        return *this;
    }

    ExplainPrinterImpl& separator(const std::string& /*separator*/) {
        // Ignored.
        return *this;
    }

private:
    ExplainPrinterImpl& printStringInternal(StringData s) {
        auto [tag, val] = sbe::value::makeNewString(s);
        addValue(tag, val);
        return *this;
    }

    ExplainPrinterImpl& fieldNameInternal(const std::string& name,
                                          const ExplainVersion minVersion,
                                          const ExplainVersion maxVersion) {
        if (minVersion <= version && maxVersion >= version) {
            _nextFieldName = name;
        }
        return *this;
    }

    ExplainPrinterImpl& print(ExplainPrinterImpl& other, const bool append) {
        auto [tag, val] = other.moveValue();
        addValue(tag, val, append);
        if (append) {
            sbe::value::releaseValue(tag, val);
        }
        return *this;
    }

    ExplainPrinterImpl& print(std::vector<ExplainPrinterImpl>& other, const bool append) {
        auto [tag, val] = sbe::value::makeNewArray();
        sbe::value::Array* arr = sbe::value::getArrayView(val);
        for (auto&& element : other) {
            auto [tag1, val1] = element.moveValue();
            arr->push_back(tag1, val1);
        }
        addValue(tag, val, append);
        return *this;
    }

    void addValue(sbe::value::TypeTags tag, sbe::value::Value val, const bool append = false) {
        if (!_initialized) {
            _initialized = true;
            _canAppend = _nextFieldName.has_value();
            if (_canAppend) {
                std::tie(_tag, _val) = sbe::value::makeNewObject();
            } else {
                _tag = tag;
                _val = val;
                return;
            }
        }

        if (!_canAppend) {
            uasserted(6624072, "Cannot append to scalar");
            return;
        }

        if (append) {
            uassert(6624073, "Field name is not set", !_nextFieldName.has_value());
            uassert(6624349,
                    "Other printer does not contain Object",
                    tag == sbe::value::TypeTags::Object);
            sbe::value::Object* obj = sbe::value::getObjectView(val);
            for (size_t i = 0; i < obj->size(); i++) {
                const auto field = obj->getAt(i);
                auto [fieldTag, fieldVal] = sbe::value::copyValue(field.first, field.second);
                addField(obj->field(i), fieldTag, fieldVal);
            }
        } else {
            tassert(6751700, "Missing field name to serialize", _nextFieldName);
            addField(*_nextFieldName, tag, val);
            _nextFieldName = boost::none;
        }
    }

    void addField(const std::string& fieldName, sbe::value::TypeTags tag, sbe::value::Value val) {
        uassert(6624075, "Duplicate field name", _fieldNameSet.insert(fieldName).second);
        sbe::value::getObjectView(_val)->push_back(fieldName, tag, val);
    }

    void reset() {
        _nextFieldName = boost::none;
        _initialized = false;
        _canAppend = false;
        _tag = sbe::value::TypeTags::Nothing;
        _val = 0;
        _fieldNameSet.clear();
    }

    // Cannot assume empty means non-existent, so use optional<>.
    boost::optional<std::string> _nextFieldName;
    bool _initialized;
    bool _canAppend;
    sbe::value::TypeTags _tag;
    sbe::value::Value _val;
    // For debugging.
    opt::unordered_set<std::string> _fieldNameSet;
};

template <const ExplainVersion version = kDefaultExplainVersion>
class ExplainGeneratorTransporter {
public:
    using ExplainPrinter = ExplainPrinterImpl<version>;

    ExplainGeneratorTransporter(bool displayProperties = false,
                                const NodeToGroupPropsMap& nodeMap = {},
                                const boost::optional<const NodeCEMap&>& nodeCEMap = boost::none)
        : _displayProperties(displayProperties), _nodeMap(nodeMap), _nodeCEMap(nodeCEMap) {
        uassert(6624005,
                "Displaying properties not supported, in the process of being deleted",
                !_displayProperties);
    }

    /**
     * Helper function that appends the logical and physical properties of 'node' nested under a new
     * field named 'properties'. Only applicable for BSON explain, for other versions this is a
     * no-op.
     */
    void maybePrintProps(ExplainPrinter& nodePrinter, const Node& node) {
        tassert(6701800,
                "Cannot have both _displayProperties and _nodeCEMap set.",
                !(_displayProperties && _nodeCEMap));
        if (_nodeCEMap || !_displayProperties || _nodeMap.empty()) {
            return;
        }
        auto it = _nodeMap.find(&node);
        uassert(6624006, "Failed to find node properties", it != _nodeMap.end());

        const NodeProps& props = it->second;

        ExplainPrinter propsPrinter;
        propsPrinter.fieldName("cost")
            .print(props._cost.getCost())
            .separator(", ")
            .fieldName("localCost")
            .print(props._localCost.getCost())
            .separator(", ")
            .fieldName("adjustedCE")
            .print(props._adjustedCE)
            .separator(", ")
            .fieldName("planNodeID")
            .print(props._planNodeId)
            .separator(", ");
        ExplainPrinter res;
        res.fieldName("properties").print(propsPrinter);
        nodePrinter.printAppend(res);
    }

    void nodeCEPropsPrint(ExplainPrinter& nodePrinter,
                          const ABT::reference_type n,
                          const Node& node) {
        tassert(6701801,
                "Cannot have both _displayProperties and _nodeCEMap set.",
                !(_displayProperties && _nodeCEMap));
        // Only allow in V2 and V3 explain. No point in printing CE when we have a delegator
        // node.
        if (!_nodeCEMap || version == ExplainVersion::V1) {
            return;
        }
        auto it = _nodeCEMap->find(&node);
        uassert(6701802, "Failed to find node ce", it != _nodeCEMap->end());
        const CEType ce = it->second;

        ExplainPrinter propsPrinter;
        propsPrinter.fieldName("ce").print(ce);
        nodePrinter.printAppend(propsPrinter);
    }

    static void printBooleanFlag(ExplainPrinter& printer,
                                 const std::string& name,
                                 const bool flag,
                                 const bool addComma = true) {
        if constexpr (version < ExplainVersion::V3) {
            if (flag) {
                if (addComma) {
                    printer.print(", ");
                }
                printer.print(name);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            printer.fieldName(name).print(flag);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    static void printDirectToParentHelper(const bool directToParent,
                                          ExplainPrinter& parent,
                                          std::function<void(ExplainPrinter& printer)> fn) {
        if (directToParent) {
            fn(parent);
        } else {
            ExplainPrinter printer;
            fn(printer);
            parent.printAppend(printer);
        }
    }

    template <class T>
    static void printProjectionsUnordered(ExplainPrinter& printer, const T& projections) {
        if constexpr (version < ExplainVersion::V3) {
            if (!projections.empty()) {
                printer.separator("{");
                bool first = true;
                for (const ProjectionName& projectionName : projections) {
                    if (first) {
                        first = false;
                    } else {
                        printer.separator(", ");
                    }
                    printer.print(projectionName);
                }
                printer.separator("}");
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (const ProjectionName& projectionName : projections) {
                ExplainPrinter local;
                local.print(projectionName);
                printers.push_back(std::move(local));
            }
            printer.print(printers);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    template <class T>
    static void printProjectionsOrdered(ExplainPrinter& printer, const T& projections) {
        ProjectionNameOrderedSet projectionSet(projections.cbegin(), projections.cend());
        printProjectionsUnordered(printer, projectionSet);
    }

    static void printProjection(ExplainPrinter& printer, const ProjectionName& projection) {
        printProjectionsUnordered(printer, ProjectionNameVector{projection});
    }

    static void printCorrelatedProjections(ExplainPrinter& printer,
                                           const ProjectionNameSet& projections) {
        printer.fieldName("correlatedProjections", ExplainVersion::V3);
        printProjectionsOrdered(printer, projections);
    }

    /**
     * Nodes
     */
    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const References& references,
                             std::vector<ExplainPrinter> inResults) {
        ExplainPrinter printer;
        if constexpr (version < ExplainVersion::V3) {
            // The ref block is redundant for V1 and V2. We typically explain the references in the
            // blocks ([]) of the individual elements.
        } else if constexpr (version == ExplainVersion::V3) {
            printer.printAppend(inResults);
        } else {
            MONGO_UNREACHABLE;
        }
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const ExpressionBinder& binders,
                             std::vector<ExplainPrinter> inResults) {
        ExplainPrinter printer;
        if constexpr (version < ExplainVersion::V3) {
            // The bind block is redundant for V1-V2 type explains, as the bound projections can be
            // inferred from the field projection map; so here we print nothing.
            return printer;
        } else if constexpr (version == ExplainVersion::V3) {
            std::map<ProjectionName, ExplainPrinter> ordered;
            for (size_t idx = 0; idx < inResults.size(); ++idx) {
                ordered.emplace(binders.names()[idx], std::move(inResults[idx]));
            }
            printer.separator("BindBlock:");
            for (auto& [name, child] : ordered) {
                printer.separator(" ").fieldName(name).print(child);
            }
        } else {
            MONGO_UNREACHABLE;
        }
        return printer;
    }

    static void printFieldProjectionMap(ExplainPrinter& printer, const FieldProjectionMap& map) {
        std::map<FieldNameType, ProjectionName> ordered;
        if (const auto& projName = map._ridProjection) {
            ordered.emplace("<rid>", *projName);
        }
        if (const auto& projName = map._rootProjection) {
            ordered.emplace("<root>", *projName);
        }
        for (const auto& entry : map._fieldProjections) {
            ordered.insert(entry);
        }

        if constexpr (version < ExplainVersion::V3) {
            bool first = true;
            for (const auto& [fieldName, projectionName] : ordered) {
                if (first) {
                    first = false;
                } else {
                    printer.print(", ");
                }
                printer.print("'").print(fieldName).print("': ").print(projectionName);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            ExplainPrinter local;
            for (const auto& [fieldName, projectionName] : ordered) {
                local.fieldName(fieldName).print(projectionName);
            }
            printer.fieldName("fieldProjectionMap").print(local);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const ScanNode& node,
                             ExplainPrinter bindResult) {
        ExplainPrinter printer("Scan");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("scanDefName", ExplainVersion::V3)
            .print(node.getScanDefName());

        if constexpr (version < ExplainVersion::V3) {
            printer.separator(", ");
            printProjection(printer, node.getProjectionName());
        }
        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);
        printer.fieldName("bindings", ExplainVersion::V3).print(bindResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const PhysicalScanNode& node,
                             ExplainPrinter bindResult) {
        ExplainPrinter printer("PhysicalScan");
        maybePrintProps(printer, node);
        printer.separator(" [{");
        printFieldProjectionMap(printer, node.getFieldProjectionMap());
        printer.separator("}, ")
            .fieldName("scanDefName", ExplainVersion::V3)
            .print(node.getScanDefName());
        printBooleanFlag(printer, "parallel", node.useParallelScan());

        // If the scan order is forward, only print it for V3. Otherwise, print for all versions.
        if (version >= ExplainVersion::V3 || node.getScanOrder() != ScanOrder::Forward) {
            printer.separator(", ");
            printer.fieldName("direction", ExplainVersion::V3)
                .print(toStringData(node.getScanOrder()));
        }

        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);
        printer.fieldName("bindings", ExplainVersion::V3).print(bindResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const ValueScanNode& node,
                             ExplainPrinter bindResult) {
        ExplainPrinter valuePrinter = generate(node.getValueArray());

        // Specifically not printing optional logical properties here. They can be displayed with
        // the properties explain.
        ExplainPrinter printer("ValueScan");
        maybePrintProps(printer, node);
        printer.separator(" [");
        printBooleanFlag(printer, "hasRID", node.getHasRID());
        printer.fieldName("arraySize").print(node.getArraySize()).separator("]");
        nodeCEPropsPrint(printer, n, node);
        printer.fieldName("values", ExplainVersion::V3)
            .print(valuePrinter)
            .fieldName("bindings", ExplainVersion::V3)
            .print(bindResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n, const CoScanNode& node) {
        ExplainPrinter printer("CoScan");
        maybePrintProps(printer, node);
        printer.separator(" []");
        nodeCEPropsPrint(printer, n, node);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const IndexScanNode& node,
                             ExplainPrinter bindResult) {
        ExplainPrinter printer("IndexScan");
        maybePrintProps(printer, node);
        printer.separator(" [{");
        printFieldProjectionMap(printer, node.getFieldProjectionMap());
        printer.separator("}, ");

        printer.fieldName("scanDefName")
            .print(node.getScanDefName())
            .separator(", ")
            .fieldName("indexDefName")
            .print(node.getIndexDefName())
            .separator(", ");

        printBooleanFlag(printer, "reversed", node.isIndexReverseOrder());

        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);
        printer.fieldName("bindings", ExplainVersion::V3).print(bindResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const SeekNode& node,
                             ExplainPrinter bindResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Seek");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("ridProjection")
            .print(node.getRIDProjectionName())
            .separator(", {");
        printFieldProjectionMap(printer, node.getFieldProjectionMap());
        printer.separator("}, ")
            .fieldName("scanDefName", ExplainVersion::V3)
            .print(node.getScanDefName())
            .separator("]");
        nodeCEPropsPrint(printer, n, node);

        printer.setChildCount(2)
            .fieldName("bindings", ExplainVersion::V3)
            .print(bindResult)
            .fieldName("references", ExplainVersion::V3)
            .print(refsResult);

        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const FilterNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter filterResult) {
        ExplainPrinter printer("Filter");
        maybePrintProps(printer, node);
        printer.separator(" []");
        nodeCEPropsPrint(printer, n, node);
        printer.setChildCount(2)
            .fieldName("filter", ExplainVersion::V3)
            .print(filterResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const EvaluationNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter projectionResult) {
        ExplainPrinter printer("Evaluation");
        maybePrintProps(printer, node);

        if constexpr (version < ExplainVersion::V3) {
            const ABT& expr = node.getProjection();

            printer.separator(" [");
            // The bind block (projectionResult) is empty in V1-V2 explains. In the case of the
            // Evaluation node, the bind block may have useful information about the embedded
            // expression, so we make sure to print the projected expression.
            printProjection(printer, node.getProjectionName());
            if (const auto ref = getTrivialExprPtr<EvalPath>(expr); !ref.empty()) {
                ExplainPrinter local = generate(ref);
                printer.separator(" = ").printSingleLevel(local).separator("]");

                nodeCEPropsPrint(printer, n, node);
                printer.setChildCount(1, true /*noInline*/);
            } else {
                printer.separator("]");

                nodeCEPropsPrint(printer, n, node);
                printer.setChildCount(2);

                auto pathPrinter = generate(expr);
                printer.print(pathPrinter);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            nodeCEPropsPrint(printer, n, node);
            printer.fieldName("projection").print(projectionResult);
        } else {
            MONGO_UNREACHABLE;
        }

        printer.fieldName("child", ExplainVersion::V3).print(childResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const RIDIntersectNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult) {
        ExplainPrinter printer("RIDIntersect");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("scanProjectionName", ExplainVersion::V3)
            .print(node.getScanProjectionName());

        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);
        printer.setChildCount(2)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const RIDUnionNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult,
                             ExplainPrinter bindResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("RIDUnion");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("scanProjectionName", ExplainVersion::V3)
            .print(node.getScanProjectionName());

        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);
        printer.setChildCount(3)
            .fieldName("bindings", ExplainVersion::V3)
            .print(bindResult)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const BinaryJoinNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult,
                             ExplainPrinter filterResult) {
        ExplainPrinter printer("BinaryJoin");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("joinType")
            .print(toStringData(node.getJoinType()))
            .separator(", ");

        printCorrelatedProjections(printer, node.getCorrelatedProjectionNames());

        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);
        printer.setChildCount(3)
            .fieldName("expression", ExplainVersion::V3)
            .print(filterResult)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    void printEqualityJoinCondition(ExplainPrinter& printer,
                                    const ProjectionNameVector& leftKeys,
                                    const ProjectionNameVector& rightKeys) {
        if constexpr (version < ExplainVersion::V3) {
            printer.print("Condition");
            for (size_t i = 0; i < leftKeys.size(); i++) {
                ExplainPrinter local;
                local.print(leftKeys.at(i)).print(" = ").print(rightKeys.at(i));
                printer.print(local);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (size_t i = 0; i < leftKeys.size(); i++) {
                ExplainPrinter local;
                local.fieldName("leftKey")
                    .print(leftKeys.at(i))
                    .fieldName("rightKey")
                    .print(rightKeys.at(i));
                printers.push_back(std::move(local));
            }
            printer.print(printers);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const HashJoinNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("HashJoin");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("joinType")
            .print(toStringData(node.getJoinType()))
            .separator("]");
        nodeCEPropsPrint(printer, n, node);

        ExplainPrinter joinConditionPrinter;
        printEqualityJoinCondition(joinConditionPrinter, node.getLeftKeys(), node.getRightKeys());

        printer.setChildCount(3)
            .fieldName("joinCondition", ExplainVersion::V3)
            .print(joinConditionPrinter)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const MergeJoinNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("MergeJoin");
        maybePrintProps(printer, node);
        printer.separator(" []");
        nodeCEPropsPrint(printer, n, node);

        ExplainPrinter joinConditionPrinter;
        printEqualityJoinCondition(joinConditionPrinter, node.getLeftKeys(), node.getRightKeys());

        ExplainPrinter collationPrinter;
        if constexpr (version < ExplainVersion::V3) {
            collationPrinter.print("Collation");
            for (const CollationOp op : node.getCollation()) {
                ExplainPrinter local;
                local.print(toStringData(op));
                collationPrinter.print(local);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (const CollationOp op : node.getCollation()) {
                ExplainPrinter local;
                local.print(toStringData(op));
                printers.push_back(std::move(local));
            }
            collationPrinter.print(printers);
        } else {
            MONGO_UNREACHABLE;
        }

        printer.setChildCount(4)
            .fieldName("joinCondition", ExplainVersion::V3)
            .print(joinConditionPrinter)
            .fieldName("collation", ExplainVersion::V3)
            .print(collationPrinter)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const SortedMergeNode& node,
                             std::vector<ExplainPrinter> childResults,
                             ExplainPrinter bindResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("SortedMerge");
        maybePrintProps(printer, node);
        printer.separator(" []");
        nodeCEPropsPrint(printer, n, node);
        printer.setChildCount(childResults.size() + 2);
        printCollationProperty(printer, node.getCollationSpec(), false /*directToParent*/);
        printer.fieldName("bindings", ExplainVersion::V3).print(bindResult);
        printer.maybeReverse().fieldName("children", ExplainVersion::V3).print(childResults);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const NestedLoopJoinNode& node,
                             ExplainPrinter leftChildResult,
                             ExplainPrinter rightChildResult,
                             ExplainPrinter filterResult) {
        ExplainPrinter printer("NestedLoopJoin");
        maybePrintProps(printer, node);
        printer.separator(" [")
            .fieldName("joinType")
            .print(toStringData(node.getJoinType()))
            .separator(", ");

        printCorrelatedProjections(printer, node.getCorrelatedProjectionNames());

        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);
        printer.setChildCount(3)
            .fieldName("expression", ExplainVersion::V3)
            .print(filterResult)
            .maybeReverse()
            .fieldName("leftChild", ExplainVersion::V3)
            .print(leftChildResult)
            .fieldName("rightChild", ExplainVersion::V3)
            .print(rightChildResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const UnionNode& node,
                             std::vector<ExplainPrinter> childResults,
                             ExplainPrinter bindResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("Union");
        maybePrintProps(printer, node);
        if constexpr (version < ExplainVersion::V3) {
            printer.separator(" [");
            printProjectionsOrdered(printer, node.binder().names());
            printer.separator("]");
        }
        nodeCEPropsPrint(printer, n, node);
        printer.setChildCount(childResults.size() + 1)
            .fieldName("bindings", ExplainVersion::V3)
            .print(bindResult)
            .maybeReverse()
            .fieldName("children", ExplainVersion::V3)
            .print(childResults);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const GroupByNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter bindAggResult,
                             ExplainPrinter refsAggResult,
                             ExplainPrinter bindGbResult,
                             ExplainPrinter refsGbResult) {
        std::map<ProjectionName, size_t> ordered;
        const ProjectionNameVector& aggProjectionNames = node.getAggregationProjectionNames();
        for (size_t i = 0; i < aggProjectionNames.size(); i++) {
            ordered.emplace(aggProjectionNames.at(i), i);
        }

        ExplainPrinter printer("GroupBy");
        maybePrintProps(printer, node);
        printer.separator(" [");

        const auto printTypeFn = [&]() {
            printer.fieldName("type", ExplainVersion::V3).print(toStringData(node.getType()));
        };
        bool displayGroupings = true;
        if constexpr (version < ExplainVersion::V3) {
            displayGroupings = false;
            const auto& gbProjNames = node.getGroupByProjectionNames();
            printProjectionsUnordered(printer, gbProjNames);
            if (node.getType() != GroupNodeType::Complete) {
                if (!gbProjNames.empty()) {
                    printer.separator(", ");
                }
                printTypeFn();
            }
        } else if constexpr (version == ExplainVersion::V3) {
            printTypeFn();
        } else {
            MONGO_UNREACHABLE;
        }

        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);

        std::vector<ExplainPrinter> aggPrinters;
        for (const auto& [projectionName, index] : ordered) {
            ExplainPrinter local;
            local.separator("[")
                .fieldName("projectionName", ExplainVersion::V3)
                .print(projectionName)
                .separator("]");
            ExplainPrinter aggExpr = generate(node.getAggregationExpressions().at(index));
            local.fieldName("aggregation", ExplainVersion::V3).print(aggExpr);
            aggPrinters.push_back(std::move(local));
        }

        ExplainPrinter gbPrinter;
        if (displayGroupings) {
            gbPrinter.fieldName("groupings").print(refsGbResult);
        }

        ExplainPrinter aggPrinter;
        aggPrinter.fieldName("aggregations").print(aggPrinters);

        printer.setChildCount(3)
            .printAppend(gbPrinter)
            .printAppend(aggPrinter)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const UnwindNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter bindResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Unwind");
        maybePrintProps(printer, node);
        printer.separator(" [");

        if constexpr (version < ExplainVersion::V3) {
            printProjectionsUnordered(
                printer,
                ProjectionNameVector{node.getProjectionName(), node.getPIDProjectionName()});
        }

        printBooleanFlag(printer, "retainNonArrays", node.getRetainNonArrays(), true /*addComma*/);
        printer.separator("]");
        nodeCEPropsPrint(printer, n, node);

        printer.setChildCount(2)
            .fieldName("bind", ExplainVersion::V3)
            .print(bindResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);
        return printer;
    }

    static void printCollationProperty(ExplainPrinter& parent,
                                       const ProjectionCollationSpec& spec,
                                       const bool directToParent) {
        std::vector<ExplainPrinter> propPrinters;
        for (const auto& entry : spec) {
            ExplainPrinter local;
            local.fieldName("projectionName", ExplainVersion::V3)
                .print(entry.first)
                .separator(": ")
                .fieldName("collationOp", ExplainVersion::V3)
                .print(toStringData(entry.second));
            propPrinters.push_back(std::move(local));
        }

        printDirectToParentHelper(directToParent, parent, [&](ExplainPrinter& printer) {
            printer.fieldName("collation").print(propPrinters);
        });
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const UniqueNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter /*refsResult*/) {
        ExplainPrinter printer("Unique");
        maybePrintProps(printer, node);

        if constexpr (version < ExplainVersion::V3) {
            printer.separator(" [");
            printProjectionsOrdered(printer, node.getProjections());
            printer.separator("]");

            nodeCEPropsPrint(printer, n, node);
            printer.setChildCount(1, true /*noInline*/);
        } else if constexpr (version == ExplainVersion::V3) {
            nodeCEPropsPrint(printer, n, node);
            printPropertyProjections(printer, node.getProjections(), false /*directToParent*/);
        } else {
            MONGO_UNREACHABLE;
        }

        printer.fieldName("child", ExplainVersion::V3).print(childResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const SpoolProducerNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter filterResult,
                             ExplainPrinter bindResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("SpoolProducer");
        maybePrintProps(printer, node);

        printer.separator(" [")
            .fieldName("type", ExplainVersion::V3)
            .print(toStringData(node.getType()))
            .separator(", ")
            .fieldName("id")
            .print(node.getSpoolId());
        if constexpr (version < ExplainVersion::V3) {
            printer.separator(", ");
            printProjectionsOrdered(printer, node.binder().names());
        }
        printer.separator("]");

        nodeCEPropsPrint(printer, n, node);
        printer.setChildCount(3);
        printer.fieldName("filter", ExplainVersion::V3).print(filterResult);
        printer.fieldName("bindings", ExplainVersion::V3).print(bindResult);
        printer.fieldName("child", ExplainVersion::V3).print(childResult);

        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const SpoolConsumerNode& node,
                             ExplainPrinter bindResult) {
        ExplainPrinter printer("SpoolConsumer");
        maybePrintProps(printer, node);

        printer.separator(" [")
            .fieldName("type", ExplainVersion::V3)
            .print(toStringData(node.getType()))
            .separator(", ")
            .fieldName("id")
            .print(node.getSpoolId());
        if constexpr (version < ExplainVersion::V3) {
            printer.separator(", ");
            printProjectionsOrdered(printer, node.binder().names());
        }
        printer.separator("]");

        nodeCEPropsPrint(printer, n, node);
        printer.fieldName("bindings", ExplainVersion::V3).print(bindResult);

        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const CollationNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Collation");
        maybePrintProps(printer, node);

        if constexpr (version < ExplainVersion::V3) {
            printer.separator(" [{");
            bool first = true;
            for (const auto& [projName, op] : node.getCollationSpec()) {
                if (first) {
                    first = false;
                } else {
                    printer.separator(", ");
                }
                printer.print(projName).separator(": ").print(toStringData(op));
            }
            printer.separator("}]");

            nodeCEPropsPrint(printer, n, node);
            printer.setChildCount(1, true /*noInline*/);
        } else if constexpr (version == ExplainVersion::V3) {
            nodeCEPropsPrint(printer, n, node);
            printCollationProperty(printer, node.getCollationSpec(), false /*directToParent*/);
            printer.fieldName("references", ExplainVersion::V3).print(refsResult);
        } else {
            MONGO_UNREACHABLE;
        }

        printer.fieldName("child", ExplainVersion::V3).print(childResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const LimitSkipNode& node,
                             ExplainPrinter childResult) {
        ExplainPrinter printer("LimitSkip");
        maybePrintProps(printer, node);
        printer.separator(" [");

        // If we have version < V3, inline the limit skip.
        if constexpr (version < ExplainVersion::V3) {
            printer.fieldName("limit");
            auto limit = node.getLimit();
            if (limit == LimitSkipNode::kMaxVal) {
                printer.print("(none)");
            } else {
                printer.print(limit);
            }
            printer.separator(", ").fieldName("skip").print(node.getSkip()).separator("]");
            nodeCEPropsPrint(printer, n, node);
            // Do not inline LimitSkip, since it's not a path.
            printer.setChildCount(1, true /*noInline*/);
        } else if (version == ExplainVersion::V3) {
            printer.separator("]");
            nodeCEPropsPrint(printer, n, node);
            printer.setChildCount(2);
            printer.fieldName("limit");
            auto limit = node.getLimit();
            if (limit == LimitSkipNode::kMaxVal) {
                printer.print("(none)");
            } else {
                printer.print(limit);
            }
            printer.separator(", ").fieldName("skip").print(node.getSkip()).separator("]");
        } else {
            MONGO_UNREACHABLE;
        }

        printer.fieldName("child", ExplainVersion::V3).print(childResult);

        return printer;
    }

    static void printPropertyProjections(ExplainPrinter& parent,
                                         const ProjectionNameVector& projections,
                                         const bool directToParent) {
        std::vector<ExplainPrinter> printers;
        for (const ProjectionName& projection : projections) {
            ExplainPrinter local;
            local.print(projection);
            printers.push_back(std::move(local));
        }

        printDirectToParentHelper(directToParent, parent, [&](ExplainPrinter& printer) {
            printer.fieldName("projections");
            if (printers.empty()) {
                ExplainPrinter dummy;
                printer.print(dummy);
            } else {
                printer.print(printers);
            }
        });
    }

    static void printDistributionProperty(ExplainPrinter& parent,
                                          const DistributionRequirement& property,
                                          const bool directToParent) {
        const auto& distribAndProjections = property.getDistributionAndProjections();

        ExplainPrinter typePrinter;
        typePrinter.fieldName("type").print(toStringData(distribAndProjections._type));

        printBooleanFlag(typePrinter, "disableExchanges", property.getDisableExchanges());

        const bool hasProjections = !distribAndProjections._projectionNames.empty();
        ExplainPrinter projectionPrinter;
        if (hasProjections) {
            printPropertyProjections(
                projectionPrinter, distribAndProjections._projectionNames, true /*directToParent*/);
            typePrinter.printAppend(projectionPrinter);
        }

        printDirectToParentHelper(directToParent, parent, [&](ExplainPrinter& printer) {
            printer.fieldName("distribution").print(typePrinter);
        });
    }

    static void printProjectionRequirementProperty(
        ExplainPrinter& parent,
        const ProjectionNameOrderPreservingSet& projections,
        const bool directToParent) {
        printPropertyProjections(parent, projections.getVector(), directToParent);
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const ExchangeNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Exchange");
        maybePrintProps(printer, node);
        printer.separator(" []");
        nodeCEPropsPrint(printer, n, node);

        printer.setChildCount(3);
        printDistributionProperty(printer, node.getProperty(), false /*directToParent*/);
        printer.fieldName("references", ExplainVersion::V3)
            .print(refsResult)
            .fieldName("child", ExplainVersion::V3)
            .print(childResult);

        return printer;
    }

    template <class P, class V, class C>
    static ExplainPrinter printProps(const std::string& description, const C& props) {
        ExplainPrinter printer;
        if constexpr (version < ExplainVersion::V3) {
            printer.print(description).print(":");
        }

        std::map<typename P::key_type, P> ordered;
        for (const auto& entry : props) {
            ordered.insert(entry);
        }

        ExplainPrinter local;
        V visitor(local);
        for (const auto& entry : ordered) {
            entry.second.visit(visitor);
        }
        printer.print(local);

        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type n,
                             const RootNode& node,
                             ExplainPrinter childResult,
                             ExplainPrinter refsResult) {
        ExplainPrinter printer("Root");
        maybePrintProps(printer, node);

        if constexpr (version < ExplainVersion::V3) {
            printer.separator(" [");
            printProjectionsOrdered(printer, node.getProjections().getVector());
            printer.separator("]");
            nodeCEPropsPrint(printer, n, node);
            printer.setChildCount(1, true /*noInline*/);
        } else if constexpr (version == ExplainVersion::V3) {
            nodeCEPropsPrint(printer, n, node);
            printer.setChildCount(3);
            printProjectionRequirementProperty(
                printer, node.getProjections(), false /*directToParent*/);
            printer.fieldName("references", ExplainVersion::V3).print(refsResult);
        } else {
            MONGO_UNREACHABLE;
        }

        printer.fieldName("child", ExplainVersion::V3).print(childResult);
        return printer;
    }

    /**
     * Expressions
     */
    ExplainPrinter transport(const ABT::reference_type /*n*/, const Blackhole& expr) {
        ExplainPrinter printer("Blackhole");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const Constant& expr) {
        ExplainPrinter printer("Const");
        printer.separator(" [").fieldName("tag", ExplainVersion::V3);

        if (version == ExplainVersion::V3) {
            std::stringstream ss;
            ss << expr.get().first;
            std::string tagAsString = ss.str();
            printer.print(tagAsString);
        }

        printer.fieldName("value", ExplainVersion::V3).print(expr.get()).separator("]");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const Variable& expr) {
        ExplainPrinter printer("Variable");
        printer.separator(" [")
            .fieldName("name", ExplainVersion::V3)
            .print(expr.name())
            .separator("]");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const UnaryOp& expr,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("UnaryOp");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(toStringData(expr.op()))
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const BinaryOp& expr,
                             ExplainPrinter leftResult,
                             ExplainPrinter rightResult) {
        ExplainPrinter printer("BinaryOp");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(toStringData(expr.op()))
            .separator("]")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("left", ExplainVersion::V3)
            .print(leftResult)
            .fieldName("right", ExplainVersion::V3)
            .print(rightResult);
        return printer;
    }


    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const If& expr,
                             ExplainPrinter condResult,
                             ExplainPrinter thenResult,
                             ExplainPrinter elseResult) {
        ExplainPrinter printer("If");
        printer.separator(" []")
            .setChildCount(3)
            .maybeReverse()
            .fieldName("condition", ExplainVersion::V3)
            .print(condResult)
            .fieldName("then", ExplainVersion::V3)
            .print(thenResult)
            .fieldName("else", ExplainVersion::V3)
            .print(elseResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const Let& expr,
                             ExplainPrinter bindResult,
                             ExplainPrinter exprResult) {
        ExplainPrinter printer("Let");
        printer.separator(" [")
            .fieldName("variable", ExplainVersion::V3)
            .print(expr.varName())
            .separator("]")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("bind", ExplainVersion::V3)
            .print(bindResult)
            .fieldName("expression", ExplainVersion::V3)
            .print(exprResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const LambdaAbstraction& expr,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("LambdaAbstraction");
        printer.separator(" [")
            .fieldName("variable", ExplainVersion::V3)
            .print(expr.varName())
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const LambdaApplication& expr,
                             ExplainPrinter lambdaResult,
                             ExplainPrinter argumentResult) {
        ExplainPrinter printer("LambdaApplication");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("lambda", ExplainVersion::V3)
            .print(lambdaResult)
            .fieldName("argument", ExplainVersion::V3)
            .print(argumentResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const FunctionCall& expr,
                             std::vector<ExplainPrinter> argResults) {
        ExplainPrinter printer("FunctionCall");
        printer.separator(" [")
            .fieldName("name", ExplainVersion::V3)
            .print(expr.name())
            .separator("]");
        if (!argResults.empty()) {
            printer.setChildCount(argResults.size())
                .maybeReverse()
                .fieldName("arguments", ExplainVersion::V3)
                .print(argResults);
        }
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const EvalPath& expr,
                             ExplainPrinter pathResult,
                             ExplainPrinter inputResult) {
        ExplainPrinter printer("EvalPath");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("path", ExplainVersion::V3)
            .print(pathResult)
            .fieldName("input", ExplainVersion::V3)
            .print(inputResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const EvalFilter& expr,
                             ExplainPrinter pathResult,
                             ExplainPrinter inputResult) {
        ExplainPrinter printer("EvalFilter");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("path", ExplainVersion::V3)
            .print(pathResult)
            .fieldName("input", ExplainVersion::V3)
            .print(inputResult);
        return printer;
    }

    /**
     * Paths
     */
    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathConstant& path,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("PathConstant");
        printer.separator(" []")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathLambda& path,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("PathLambda");
        printer.separator(" []")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const PathIdentity& path) {
        ExplainPrinter printer("PathIdentity");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathDefault& path,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("PathDefault");
        printer.separator(" []")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathCompare& path,
                             ExplainPrinter valueResult) {
        ExplainPrinter printer("PathCompare");
        printer.separator(" [")
            .fieldName("op", ExplainVersion::V3)
            .print(toStringData(path.op()))
            .separator("]")
            .setChildCount(1)
            .fieldName("value", ExplainVersion::V3)
            .print(valueResult);
        return printer;
    }

    static void printPathProjections(ExplainPrinter& printer, const FieldNameOrderedSet& names) {
        if constexpr (version < ExplainVersion::V3) {
            bool first = true;
            for (const FieldNameType& s : names) {
                if (first) {
                    first = false;
                } else {
                    printer.print(", ");
                }
                printer.print(s);
            }
        } else if constexpr (version == ExplainVersion::V3) {
            std::vector<ExplainPrinter> printers;
            for (const FieldNameType& s : names) {
                ExplainPrinter local;
                local.print(s);
                printers.push_back(std::move(local));
            }
            printer.fieldName("projections").print(printers);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const PathDrop& path) {
        ExplainPrinter printer("PathDrop");
        printer.separator(" [");
        printPathProjections(printer, path.getNames());
        printer.separator("]");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const PathKeep& path) {
        ExplainPrinter printer("PathKeep");
        printer.separator(" [");
        printPathProjections(printer, path.getNames());
        printer.separator("]");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const PathObj& path) {
        ExplainPrinter printer("PathObj");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const PathArr& path) {
        ExplainPrinter printer("PathArr");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathTraverse& path,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("PathTraverse");
        printer.separator(" [");

        if constexpr (version < ExplainVersion::V3) {
            if (path.getMaxDepth() == PathTraverse::kUnlimited) {
                printer.print("inf");
            } else {
                printer.print(path.getMaxDepth());
            }
        } else if constexpr (version == ExplainVersion::V3) {
            printer.fieldName("maxDepth", ExplainVersion::V3).print(path.getMaxDepth());
        } else {
            MONGO_UNREACHABLE;
        }

        printer.separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathField& path,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("PathField");
        printer.separator(" [")
            .fieldName("path", ExplainVersion::V3)
            .print(path.name())
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathGet& path,
                             ExplainPrinter inResult) {
        ExplainPrinter printer("PathGet");
        printer.separator(" [")
            .fieldName("path", ExplainVersion::V3)
            .print(path.name())
            .separator("]")
            .setChildCount(1)
            .fieldName("input", ExplainVersion::V3)
            .print(inResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathComposeM& path,
                             ExplainPrinter leftResult,
                             ExplainPrinter rightResult) {
        ExplainPrinter printer("PathComposeM");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("leftInput", ExplainVersion::V3)
            .print(leftResult)
            .fieldName("rightInput", ExplainVersion::V3)
            .print(rightResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/,
                             const PathComposeA& path,
                             ExplainPrinter leftResult,
                             ExplainPrinter rightResult) {
        ExplainPrinter printer("PathComposeA");
        printer.separator(" []")
            .setChildCount(2)
            .maybeReverse()
            .fieldName("leftInput", ExplainVersion::V3)
            .print(leftResult)
            .fieldName("rightInput", ExplainVersion::V3)
            .print(rightResult);
        return printer;
    }

    ExplainPrinter transport(const ABT::reference_type /*n*/, const Source& expr) {
        ExplainPrinter printer("Source");
        printer.separator(" []");
        return printer;
    }

    ExplainPrinter generate(const ABT::reference_type node) {
        return algebra::transport<true>(node, *this);
    }

private:
    const bool _displayProperties;

    // We don't own this.
    const NodeToGroupPropsMap& _nodeMap;
    boost::optional<const NodeCEMap&> _nodeCEMap;
};

using ExplainGeneratorV1 = ExplainGeneratorTransporter<ExplainVersion::V1>;
using ExplainGeneratorV2 = ExplainGeneratorTransporter<ExplainVersion::V2>;
using ExplainGeneratorV2Compact = ExplainGeneratorTransporter<ExplainVersion::V2Compact>;
using ExplainGeneratorV3 = ExplainGeneratorTransporter<ExplainVersion::V3>;

std::string ExplainGenerator::explain(const ABT::reference_type node,
                                      const bool displayProperties,
                                      const NodeToGroupPropsMap& nodeMap) {
    ExplainGeneratorV1 gen(displayProperties, nodeMap);
    return gen.generate(node).str();
}

std::string ExplainGenerator::explainV2(const ABT::reference_type node,
                                        const bool displayProperties,
                                        const NodeToGroupPropsMap& nodeMap) {
    ExplainGeneratorV2 gen(displayProperties, nodeMap);
    return gen.generate(node).str();
}

std::string ExplainGenerator::explainV2Compact(const ABT::reference_type node,
                                               const bool displayProperties,
                                               const NodeToGroupPropsMap& nodeMap) {
    ExplainGeneratorV2Compact gen(displayProperties, nodeMap);
    return gen.generate(node).str();
}

std::string ExplainGenerator::explainNode(const ABT::reference_type node) {
    if (node.empty()) {
        return "Empty\n";
    }
    return explainV2(node);
}

std::pair<sbe::value::TypeTags, sbe::value::Value> ExplainGenerator::explainBSON(
    const ABT::reference_type node,
    const bool displayProperties,
    const NodeToGroupPropsMap& nodeMap) {
    ExplainGeneratorV3 gen(displayProperties, nodeMap);
    return gen.generate(node).moveValue();
}

BSONObj convertSbeValToBSONObj(const std::pair<sbe::value::TypeTags, sbe::value::Value> val) {
    uassert(6624070, "Expected an object", val.first == sbe::value::TypeTags::Object);
    sbe::value::ValueGuard vg(val.first, val.second);

    BSONObjBuilder builder;
    sbe::bson::convertToBsonObj(builder, sbe::value::getObjectView(val.second));
    return builder.done().getOwned();
}

BSONObj ExplainGenerator::explainBSONObj(const ABT::reference_type node,
                                         const bool displayProperties,
                                         const NodeToGroupPropsMap& nodeMap) {
    return convertSbeValToBSONObj(explainBSON(node, displayProperties, nodeMap));
}

template <class PrinterType>
static void printBSONstr(PrinterType& printer,
                         const sbe::value::TypeTags tag,
                         const sbe::value::Value val) {
    switch (tag) {
        case sbe::value::TypeTags::Array: {
            const auto* array = sbe::value::getArrayView(val);

            PrinterType local;
            for (size_t index = 0; index < array->size(); index++) {
                if (index > 0) {
                    local.print(", ");
                    local.newLine();
                }
                const auto [tag1, val1] = array->getAt(index);
                printBSONstr(local, tag1, val1);
            }
            printer.print("[").print(local).print("]");

            break;
        }

        case sbe::value::TypeTags::Object: {
            const auto* obj = sbe::value::getObjectView(val);

            PrinterType local;
            for (size_t index = 0; index < obj->size(); index++) {
                if (index > 0) {
                    local.print(", ");
                    local.newLine();
                }
                local.fieldName(obj->field(index));
                const auto [tag1, val1] = obj->getAt(index);
                printBSONstr(local, tag1, val1);
            }
            printer.print("{").print(local).print("}");

            break;
        }

        default: {
            std::ostringstream os;
            os << std::make_pair(tag, val);
            printer.print(os.str());
        }
    }
}

std::string ExplainGenerator::explainBSONStr(const ABT::reference_type node,
                                             bool displayProperties,
                                             const NodeToGroupPropsMap& nodeMap) {
    const auto [tag, val] = explainBSON(node, displayProperties, nodeMap);
    sbe::value::ValueGuard vg(tag, val);
    ExplainPrinterImpl<ExplainVersion::V2> printer;
    printBSONstr(printer, tag, val);
    return printer.str();
}

class ShortPlanSummaryTransport {
public:
    void transport(const PhysicalScanNode& node, const ABT&) {
        ss << "COLLSCAN";
    }

    void transport(const IndexScanNode& node, const ABT&) {
        std::string idxCombined = getIndexDetails(node);
        if (ss.str().find(idxCombined) == std::string::npos) {
            if (ss.tellp() != 0) {
                ss << ", ";
            }
            ss << idxCombined;
        }
    }

    std::string getIndexDetails(const IndexScanNode& node) {
        std::stringstream idxDetails;
        idxDetails << "IXSCAN { " << node.getIndexDefName() << " }";
        return idxDetails.str();
    }

    template <typename T, typename... Ts>
    void transport(const T& node, Ts&&...) {
        static_assert(
            (!std::is_base_of_v<PhysicalScanNode, T>)&&(!std::is_base_of_v<IndexScanNode, T>));
    }

    std::string getPlanSummary(const ABT& n) {
        if (isEOFPlan(n)) {
            return "EOF";
        }

        algebra::transport<false>(n, *this);
        return ss.str();
    }

    std::stringstream ss;
};

std::string ABTPrinter::getPlanSummary() const {
    return ShortPlanSummaryTransport().getPlanSummary(_planAndProps._node);
}

bool isEOFPlan(const ABT::reference_type node) {
    // This function expects the full ABT to be the argument. So we must have a RootNode.
    auto root = node.cast<RootNode>();
    if (!root->getChild().is<EvaluationNode>()) {
        // An EOF plan will have an EvaluationNode as the child of the RootNode.
        return false;
    }

    auto eval = root->getChild().cast<EvaluationNode>();
    if (eval->getProjection() != Constant::nothing()) {
        // The EvaluationNode of an EOF plan will have Nothing as the projection.
        return false;
    }

    // This is the rest of an EOF plan.
    ABT eofChild = make<LimitSkipNode>(0, 0, make<CoScanNode>());
    return eval->getChild() == eofChild;
}

class StringifyPathsAndExprsTransporter {
public:
    template <typename T, typename... Ts>
    void walk(const T&, StringBuilder* sb, Ts&&...) {
        tasserted(8075801,
                  str::stream() << "Trying to stringify an unsupported operator for explain: "
                                << boost::core::demangle(typeid(T).name()));
    }

    // Helpers
    std::string prettyPrintPathProjs(const FieldNameOrderedSet& names) {
        StringBuilder result;
        bool first = true;
        for (const FieldNameType& s : names) {
            if (first) {
                first = false;
            } else {
                result.append(", ");
            }
            result.append(s.value());
        }
        return result.str();
    }

    void generateStringForLeafNode(StringBuilder* sb,
                                   StringData name,
                                   boost::optional<StringData> property) {
        sb->append(std::move(name));

        if (property) {
            sb->append(" [");
            sb->append(std::move(property.get()));
            sb->append("]");
        }
    }

    void generateStringForOneChildNode(StringBuilder* sb,
                                       StringData name,
                                       boost::optional<StringData> property,
                                       const ABT& child,
                                       bool addParensAroundChild = false) {
        sb->append(std::move(name));

        if (property) {
            sb->append(" [");
            sb->append(std::move(property.get()));
            sb->append("] ");
        } else {
            sb->append(" ");
        }

        if (addParensAroundChild) {
            sb->append("(");
        }

        generateString(child, sb);


        if (addParensAroundChild) {
            sb->append(")");
        }
    }

    void generateStringForTwoChildNode(StringBuilder* sb,
                                       StringData name,
                                       const ABT& childOne,
                                       const ABT& childTwo) {
        sb->append(std::move(name));

        sb->append(" (");
        generateString(childOne, sb);
        sb->append(")");

        sb->append(" (");
        generateString(childTwo, sb);
        sb->append(")");
    }

    /**
     * Paths
     */
    void walk(const PathConstant& path, StringBuilder* sb, const ABT& child) {
        generateStringForOneChildNode(sb, "Constant" /* name */, boost::none /* property */, child);
    }

    void walk(const PathLambda& path, StringBuilder* sb, const ABT& child) {
        generateStringForOneChildNode(sb, "Lambda" /* name */, boost::none /* property */, child);
    }

    void walk(const PathIdentity& path, StringBuilder* sb) {
        generateStringForLeafNode(sb, "Identity" /* name */, boost::none /* property */);
    }

    void walk(const PathDefault& path, StringBuilder* sb, const ABT& child) {
        generateStringForOneChildNode(sb, "Default" /* name */, boost::none /* property */, child);
    }

    void walk(const PathCompare& path, StringBuilder* sb, const ABT& child) {
        std::string name;
        switch (path.op()) {
            case Operations::Eq:
                name = "=";
                break;
            case Operations::EqMember:
                name = "eqMember";
                break;
            case Operations::Neq:
                name = "!=";
                break;
            case Operations::Gt:
                name = ">";
                break;
            case Operations::Gte:
                name = ">=";
                break;
            case Operations::Lt:
                name = "<";
                break;
            case Operations::Lte:
                name = "<=";
                break;
            case Operations::Cmp3w:
                name = "<=>";
                break;
            default:
                // Instead of reaching this case, we'd first hit error code 6684500 when the
                // PathCompare was created with a non-comparison operator.
                MONGO_UNREACHABLE;
        }

        generateStringForOneChildNode(sb, name, boost::none /* property */, child);
    }

    void walk(const PathDrop& path, StringBuilder* sb) {
        generateStringForLeafNode(
            sb, "Drop" /* name */, StringData(prettyPrintPathProjs(path.getNames())));
    }

    void walk(const PathKeep& path, StringBuilder* sb) {
        generateStringForLeafNode(
            sb, "Keep" /* name */, StringData(prettyPrintPathProjs(path.getNames())));
    }

    void walk(const PathObj& path, StringBuilder* sb) {
        generateStringForLeafNode(sb, "Obj" /* name */, boost::none /* property */);
    }

    void walk(const PathArr& path, StringBuilder* sb) {
        generateStringForLeafNode(sb, "Arr" /* name */, boost::none /* property */);
    }

    void walk(const PathTraverse& path, StringBuilder* sb, const ABT& child) {
        std::string property;
        if (path.getMaxDepth() == PathTraverse::kUnlimited) {
            property = "inf";
        } else {
            // The string 'property' will own the result of ss.str() below, so it will hold the
            // value of the PathTraverse's max depth after the stringstream goes out of scope.
            std::stringstream ss;
            ss << path.getMaxDepth();
            property = ss.str();
        }
        generateStringForOneChildNode(sb, "Traverse" /* name */, StringData(property), child);
    }

    void walk(const PathField& path, StringBuilder* sb, const ABT& child) {
        generateStringForOneChildNode(sb, "Field" /* name */, path.name().value(), child);
    }

    void walk(const PathGet& path, StringBuilder* sb, const ABT& child) {
        generateStringForOneChildNode(sb, "Get" /* name */, path.name().value(), child);
    }

    void walk(const PathComposeM& path,
              StringBuilder* sb,
              const ABT& leftChild,
              const ABT& rightChild) {
        generateStringForTwoChildNode(sb, "ComposeM" /* name */, leftChild, rightChild);
    }

    void walk(const PathComposeA& path,
              StringBuilder* sb,
              const ABT& leftChild,
              const ABT& rightChild) {
        generateStringForTwoChildNode(sb, "ComposeA" /* name */, leftChild, rightChild);
    }

    /**
     * Expressions
     */
    void walk(const Constant& expr, StringBuilder* sb) {
        generateStringForLeafNode(
            sb, "Const" /* name */, StringData(sbe::value::print(expr.get())));
    }

    void walk(const Variable& expr, StringBuilder* sb) {
        generateStringForLeafNode(sb, "Var" /* name */, expr.name().value());
    }

    void walk(const UnaryOp& expr, StringBuilder* sb, const ABT& child) {
        generateStringForOneChildNode(sb,
                                      toStringData(expr.op()),
                                      boost::none /* property */,
                                      child,
                                      true /* addParensAroundChild */);
    }

    void walk(const BinaryOp& expr,
              StringBuilder* sb,
              const ABT& leftChild,
              const ABT& rightChild) {
        generateStringForTwoChildNode(sb, toStringData(expr.op()), leftChild, rightChild);
    }

    void walk(const If& expr,
              StringBuilder* sb,
              const ABT& condChild,
              const ABT& thenChild,
              const ABT& elseChild) {
        sb->append("if");
        sb->append(" (");
        generateString(condChild, sb);
        sb->append(") ");

        sb->append("then");
        sb->append(" (");
        generateString(thenChild, sb);
        sb->append(") ");

        sb->append("else");
        sb->append(" (");
        generateString(elseChild, sb);
        sb->append(")");
    }

    void walk(const Let& expr, StringBuilder* sb, const ABT& bind, const ABT& in) {
        sb->append("let ");
        sb->append(expr.varName().value());

        sb->append(" = (");
        generateString(bind, sb);
        sb->append(") ");

        sb->append("in (");
        generateString(in, sb);
        sb->append(")");
    }

    void walk(const LambdaAbstraction& expr, StringBuilder* sb, const ABT& body) {
        generateStringForOneChildNode(sb,
                                      "LambdaAbstraction" /* name */,
                                      expr.varName().value(),
                                      body,
                                      true /* addParensAroundChild */);
    }

    void walk(const LambdaApplication& expr,
              StringBuilder* sb,
              const ABT& lambda,
              const ABT& argument) {
        generateStringForTwoChildNode(sb, "LambdaApplication" /* name */, lambda, argument);
    }

    void walk(const FunctionCall& expr, StringBuilder* sb, const std::vector<ABT>& args) {
        sb->append(expr.name());
        sb->append("(");

        // TODO SERVER-83824: Remvoe the special case for getParam - just include the body of the
        // else here.
        if (expr.name() == "getParam") {
            //  The getParam FunctionCall node has two children, one is the parameter id and the
            //  other is an enum/int representation of the constant's sbe type tag. For explain
            //  purposes, we want this function call to look like "getParam(<id>)" so we extract and
            //  display only the first child.
            generateString(args.at(0), sb);
        } else {
            bool first = true;
            for (const auto& arg : args) {
                if (first) {
                    first = false;
                } else {
                    sb->append(", ");
                }
                generateString(arg, sb);
            }
        }

        sb->append(")");
    }

    void walk(const EvalPath& expr, StringBuilder* sb, const ABT& path, const ABT& input) {
        generateStringForTwoChildNode(sb, "EvalPath" /* name */, path, input);
    }

    void walk(const EvalFilter& expr, StringBuilder* sb, const ABT& path, const ABT& input) {
        generateStringForTwoChildNode(sb, "EvalFilter" /* name */, path, input);
    }

    void generateString(const ABT::reference_type n, StringBuilder* sb) {
        algebra::walk<false>(n, *this, sb);
    }
};

std::string StringifyPathsAndExprs::stringify(const ABT::reference_type node) {
    StringBuilder result;
    StringifyPathsAndExprsTransporter().generateString(node, &result);
    return result.str();
}
}  // namespace mongo::optimizer