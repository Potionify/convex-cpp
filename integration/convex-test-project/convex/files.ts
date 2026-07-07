import { query, mutation } from "./_generated/server";
import { v } from "convex/values";

// Generate a short-lived URL the client can POST file bytes to.
export const generateUploadUrl = mutation({
  args: {},
  handler: async (ctx) => {
    return await ctx.storage.generateUploadUrl();
  },
});

// Resolve a storage id to a download URL (null if it does not exist).
export const getUrl = query({
  args: { storageId: v.id("_storage") },
  handler: async (ctx, { storageId }) => {
    return await ctx.storage.getUrl(storageId);
  },
});

// System metadata for an uploaded file (size, sha256, contentType).
export const getMetadata = query({
  args: { storageId: v.id("_storage") },
  handler: async (ctx, { storageId }) => {
    return await ctx.db.system.get(storageId);
  },
});
