/**
 * Test that larger queries do not fail. This includes larger aggregation pipelines, as well as
 * large $match/$project stages, deeply nested paths, and many predicates in an $and/$or.
 * The size of these queries was found by trial and error until we reach the BSON size limit.
 *
 * @tags: [
 *   # Can't wrap queries in facets without going past max BSON depth.
 *   do_not_wrap_aggregations_in_facets,
 *   not_allowed_with_signed_security_token,
 *   # Can't use multiplanning, as it leads to query serialization that fails because of max BSON
 *   # size.
 *   does_not_support_multiplanning_single_solutions,
 *   incompatible_aubsan,
 * ]
 */

(function() {
"use strict";

// Only run this test for debug=off opt=on without sanitizers active. With any of these activated,
// the stack frames are larger and can more easily stack overflow.
const buildInfo = db.getServerBuildInfo();
if (buildInfo.isDebug() || !buildInfo.isOptimizationsEnabled() ||
    buildInfo.isAddressSanitizerActive() || buildInfo.isLeakSanitizerActive() ||
    buildInfo.isThreadSanitizerActive() || buildInfo.isUndefinedBehaviorSanitizerActive() ||
    _isSpiderMonkeyDebugEnabled()) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    return;
}

const coll = db.query_limits_test;
coll.drop();

// Multikey so we can't apply any non-multikey optimizations to stress as much as possible.
assert.commandWorked(coll.insert({_id: 0, a: [0, 1], b: [2, 3], c: 4, d: 5, object: {}}));

function range(high) {
    return [...Array(high).keys()];
}

function runAgg(pipeline) {
    // Run pipeline to make sure it doesn't fail.
    const result = coll.aggregate(pipeline).toArray();
}

// Construct a {$match: {a: {$in: [0, 1, 2, ...]}}}.
function testLargeIn() {
    jsTestLog("Testing large $in");
    // Int limit is different than double limit.
    const filterValsInts = range(1200000).map(i => NumberInt(i));
    runAgg([{$match: {a: {$in: filterValsInts}}}]);

    const filterValsDoubles = range(1000000).map(i => i * 1.0);
    runAgg([{$match: {a: {$in: filterValsDoubles}}}]);
}

// Construct a {$project: {a0: 1, a1: 1, ...}}.
function testLargeProject() {
    jsTestLog("Testing large $project");
    const projectFields = {};
    range(1000000).forEach(function(i) {
        projectFields["a" + i] = NumberInt(1);
    });
    runAgg([{$project: projectFields}]);

    const pathSize = 1000000;
    let nestedProjectField = "a0";
    for (let i = 1; i < pathSize; i++) {
        nestedProjectField += ".a" + i;
    }
    runAgg([{$project: {nestedProjectField: 1}}]);
}

// Run $and and $or with many different types of predicates.
function testLargeAndOrPredicates() {
    jsTestLog("Testing large $and/$or predicates");

    // Large $match of the form {$match: {a0: 1, a1: 1, ...}}
    const largeMatch = {};
    range(800000).forEach(function(i) {
        largeMatch["a" + i] = NumberInt(1);
    });
    runAgg([{$match: largeMatch}]);

    function intStream(n) {
        return range(n).map(i => NumberInt(i));
    }

    const andOrFilters = [
        // Plain a=i filter.
        intStream(500000).map(function(i) {
            return {a: i};
        }),
        // a_i = i filter. Different field for each value.
        intStream(500000).map(function(i) {
            const field = "a" + i;
            return {[field]: i};
        }),
        // Mix of lt and gt with the same field.
        intStream(500000).map(function(i) {
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {a: predicate};
        }),
        // Mix of lt and gt with different fields.
        intStream(400000).map(function(i) {
            const field = "a" + i;
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {[field]: predicate};
        }),
        // Mix of lt and gt wrapped in not with different fields.
        intStream(300000).map(function(i) {
            const field = "a" + i;
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {[field]: {$not: predicate}};
        }),
        // $exists on different fields.
        intStream(400000).map(function(i) {
            const field = "a" + i;
            return {[field]: {$exists: true}};
        }),
        intStream(400000).map(function(i) {
            const field = "a" + i;
            return {[field]: {$exists: false}};
        })
    ];
    for (const m of andOrFilters) {
        runAgg([{$match: {$and: m}}]);
        runAgg([{$match: {$or: m}}]);
    }
}

// Test deeply nested queries.
function testDeeplyNestedPath() {
    jsTestLog("Testing deeply nested $match");
    let deepQuery = {a: {$eq: 1}};
    const depth = 72;
    for (let i = 0; i < depth; i++) {
        deepQuery = {a: {$elemMatch: deepQuery}};
    }
    runAgg([{$match: deepQuery}]);
}

// Test pipeline length.
function testPipelineLimits() {
    jsTestLog("Testing large agg pipelines");
    const pipelineLimit =
        assert.commandWorked(db.adminCommand({getParameter: 1, internalPipelineLengthLimit: 1}))
            .internalPipelineLengthLimit;
    let stages = [
        {$limit: 1},
        {$skip: 1},
        {$sort: {a: 1}},
        {$unwind: "$a"},
        {$match: {a: {$mod: [4, 2]}}},
        {$group: {_id: "$a"}},
        {$addFields: {c: {$add: ["$c", "$d"]}}},
        {$addFields: {a: 5}},
        {$project: {a: 1}},
        {$match: {a: 1}},
    ];

    for (const stage of stages) {
        const pipeline = range(pipelineLimit).map(_ => stage);
        jsTestLog(stage);
        runAgg(pipeline);
    }
}

/*
 * Generates a $match query with specified branchingFactor and maxDepth of the form
 * {$and: [{$or: [... $and ...]}, ... (length branchingFactor) ...]}
 * Uses unique field names across the generated query.
 */
let fieldIndex = 0;
function generateNestedAndOrHelper(type, branchingFactor, maxDepth) {
    if (maxDepth === 0) {
        const field = 'a' + fieldIndex;
        const query = {[field]: NumberInt(fieldIndex)};
        fieldIndex++;
        return query;
    }

    const oppositeType = type === '$and' ? '$or' : '$and';
    const children = [];
    for (let i = 0; i < branchingFactor; i++) {
        const childQuery = generateNestedAndOrHelper(oppositeType, branchingFactor, maxDepth - 1);
        children.push(childQuery);
    }

    return {[type]: children};
}

function generateNestedAndOr(type, branchingFactor, maxDepth) {
    fieldIndex = 0;
    return generateNestedAndOrHelper(type, branchingFactor, maxDepth);
}

function testNestedAndOr() {
    jsTestLog("Testing nested $and/$or");
    for (const topLevelType of ['$and', '$or']) {
        // Test different types of nested queries
        let [branchingFactor, maxDepth] = [3, 10];
        const deepNarrowQuery = generateNestedAndOr(topLevelType, branchingFactor, maxDepth);
        runAgg([{$match: deepNarrowQuery}]);

        [branchingFactor, maxDepth] = [10, 5];
        const shallowWideQuery = generateNestedAndOr(topLevelType, branchingFactor, maxDepth);
        runAgg([{$match: shallowWideQuery}]);
    }
}

function testLargeSetFunction() {
    jsTestLog("Testing large $setIntersection");

    const fieldExprs = [];
    for (let j = 1; j <= 13000; j++) {
        fieldExprs.push("$a" + j);
    }
    const pipeline = [{$project: {a: {$setIntersection: fieldExprs}}}, {$group: {_id: "$a"}}];
    runAgg(pipeline);
}

const tests = [
    testLargeIn,
    testLargeProject,
    testLargeAndOrPredicates,
    testDeeplyNestedPath,
    testNestedAndOr,
    testPipelineLimits,
    testLargeSetFunction
];

for (const test of tests) {
    test();
}
})();