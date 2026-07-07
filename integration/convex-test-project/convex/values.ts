import { query, mutation } from "./_generated/server";
import { v } from "convex/values";

// Echo the single arg `x` back unchanged (v.any covers the whole value space).
export const echoQuery = query({
  args: { x: v.any() },
  handler: async (_ctx, { x }) => {
    return x;
  },
});

// Same as echoQuery but as a mutation.
export const echoMutation = mutation({
  args: { x: v.any() },
  handler: async (_ctx, { x }) => {
    return x;
  },
});

// Build an 8-byte ArrayBuffer holding 0,1,2,3,4,5,6,7.
function bytes0to7(): ArrayBuffer {
  const arr = new Uint8Array(8);
  for (let i = 0; i < 8; i++) arr[i] = i;
  return arr.buffer;
}

// Return an object containing every Convex value type. No args.
export const kitchenSink = query({
  args: {},
  handler: async () => {
    return {
      nullValue: null,
      boolTrue: true,
      boolFalse: false,
      // Int64 / BigInt values (Convex maps bigint -> Int64).
      int64Big: 9007199254740993n,
      int64Min: -9223372036854775808n,
      // Float64 values.
      float: 1.5,
      floatZero: 0,
      // Special IEEE-754 floats live inside an array so the whole set round-trips.
      specialFloats: [NaN, Infinity, -Infinity, -0],
      // Unicode string (emoji, accents, CJK, combining marks).
      unicode: "héllo 世界 🧪✨ café ñ",
      // Bytes (ArrayBuffer).
      bytes: bytes0to7(),
      // Nested array.
      nestedArray: [1, [2, 3, [4, 5]], "six", [true, null]],
      // Nested object.
      nestedObject: {
        a: 1,
        b: { c: 2, d: { e: 3, f: [7, 8, 9] } },
        list: [{ k: "v" }, { k: "w" }],
      },
    };
  },
});
