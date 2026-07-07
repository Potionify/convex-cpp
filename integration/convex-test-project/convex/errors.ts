import { query } from "./_generated/server";
import { ConvexError } from "convex/values";

// Throws an application-level ConvexError carrying structured data.
export const throwConvexError = query({
  args: {},
  handler: async () => {
    throw new ConvexError({ code: "TEST", details: [1, "two"] });
  },
});

// Throws a plain JS Error (surfaces as a generic developer/server error).
export const throwPlainError = query({
  args: {},
  handler: async () => {
    throw new Error("plain failure");
  },
});
