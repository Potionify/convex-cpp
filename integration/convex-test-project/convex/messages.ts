import { query, mutation } from "./_generated/server";
import { paginationOptsValidator } from "convex/server";
import { v } from "convex/values";

// List messages in a channel, oldest first (ascending _creationTime).
export const list = query({
  args: { channel: v.string() },
  handler: async (ctx, { channel }) => {
    return await ctx.db
      .query("messages")
      .withIndex("by_channel", (q) => q.eq("channel", channel))
      .order("asc")
      .collect();
  },
});

// Paginated list of messages in a channel, oldest first. Exercised by the
// convex::paginated_query live tests.
export const listPaginated = query({
  args: { channel: v.string(), paginationOpts: paginationOptsValidator },
  handler: async (ctx, { channel, paginationOpts }) => {
    return await ctx.db
      .query("messages")
      .withIndex("by_channel", (q) => q.eq("channel", channel))
      .order("asc")
      .paginate(paginationOpts);
  },
});

// Send (insert) a new message. Returns the new document id.
export const send = mutation({
  args: {
    channel: v.string(),
    author: v.string(),
    body: v.string(),
  },
  handler: async (ctx, { channel, author, body }) => {
    return await ctx.db.insert("messages", { channel, author, body });
  },
});

// Delete every message. Returns the number of documents removed.
export const clearAll = mutation({
  args: {},
  handler: async (ctx) => {
    const all = await ctx.db.query("messages").collect();
    for (const doc of all) {
      await ctx.db.delete(doc._id);
    }
    return all.length;
  },
});
