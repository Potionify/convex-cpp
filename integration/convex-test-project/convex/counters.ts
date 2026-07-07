import { query, mutation } from "./_generated/server";
import { v } from "convex/values";

// Get a counter's value by name. Returns the number, or null if it does not exist.
export const get = query({
  args: { name: v.string() },
  handler: async (ctx, { name }): Promise<number | null> => {
    const doc = await ctx.db
      .query("counters")
      .withIndex("by_name", (q) => q.eq("name", name))
      .unique();
    return doc === null ? null : doc.value;
  },
});

// Increment (or create) a counter. Returns the new value.
export const increment = mutation({
  args: {
    name: v.string(),
    by: v.optional(v.number()),
  },
  handler: async (ctx, { name, by }): Promise<number> => {
    const delta = by ?? 1;
    const doc = await ctx.db
      .query("counters")
      .withIndex("by_name", (q) => q.eq("name", name))
      .unique();
    if (doc === null) {
      await ctx.db.insert("counters", { name, value: delta });
      return delta;
    }
    const next = doc.value + delta;
    await ctx.db.patch(doc._id, { value: next });
    return next;
  },
});
