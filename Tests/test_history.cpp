// PrismArchive -- PrismCore test
// History stack (blueprint ch. 9.1 row 6: Alt+Left / Alt+Right).
//
// These cases pin the two contractual properties, not just "it works":
//   * adjacent de-duplication (a crumb click on the level you are already on
//     must not consume a Back press);
//   * the capacity is exact (CONTRACTS D17): after an overflow the stack holds
//     capacity entries, never capacity-1.
// The mutation builds in Tests/CMakeLists.txt inject each property back and
// MUST go red (CONTRACTS D3).
#include <string>

#include "../Src/PrismCore/HistoryStack.hpp"
#include "mini_test.hpp"

using prism::history::Stack;

PRISM_TEST("history: starts at the archive root with nothing to go back to") {
  Stack stack(4);
  if (!PRISM_CHECK_EQ(stack.Current(), std::string(""))) return;
  if (!PRISM_CHECK(!stack.CanBack())) return;
  if (!PRISM_CHECK(!stack.CanForward())) return;
  std::string out = "sentinel";
  if (!PRISM_CHECK(!stack.Back(&out))) return;
  // A refused Back must not touch the caller's buffer: the window keeps the
  // folder it is showing and only re-renders when the call succeeded.
  if (!PRISM_CHECK_EQ(out, std::string("sentinel"))) return;
  if (!PRISM_CHECK(!stack.Forward(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("sentinel"))) return;
}

PRISM_TEST("history: visit -> back -> forward walks the exact levels") {
  Stack stack;
  if (!PRISM_CHECK(stack.Visit("docs"))) return;
  if (!PRISM_CHECK_EQ(stack.Current(), std::string("docs"))) return;
  if (!PRISM_CHECK_EQ(stack.BackCount(), size_t{1})) return;  // the root itself
  if (!PRISM_CHECK(stack.Visit("docs/manual"))) return;
  if (!PRISM_CHECK_EQ(stack.BackCount(), size_t{2})) return;

  std::string out;
  if (!PRISM_CHECK(stack.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("docs"))) return;
  if (!PRISM_CHECK_EQ(stack.Current(), std::string("docs"))) return;
  if (!PRISM_CHECK_EQ(stack.ForwardCount(), size_t{1})) return;

  if (!PRISM_CHECK(stack.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string(""))) return;  // back at the root
  if (!PRISM_CHECK(!stack.CanBack())) return;

  if (!PRISM_CHECK(stack.Forward(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("docs"))) return;
  if (!PRISM_CHECK(stack.Forward(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("docs/manual"))) return;
  if (!PRISM_CHECK(!stack.CanForward())) return;
}

PRISM_TEST("history: re-entering the current folder is a no-op (de-dupe)") {
  Stack stack;
  if (!PRISM_CHECK(stack.Visit("pack"))) return;
  if (!PRISM_CHECK(stack.Visit("pack/sub"))) return;
  const size_t backBefore = stack.BackCount();
  // The user clicks the crumb of the folder they are already in. Without the
  // de-dupe the next Back press would look broken (it would "go back" to the
  // same folder).
  if (!PRISM_CHECK(!stack.Visit("pack/sub"))) return;
  if (!PRISM_CHECK_EQ(stack.BackCount(), backBefore)) return;
  if (!PRISM_CHECK_EQ(stack.Current(), std::string("pack/sub"))) return;
  // ... and the forward branch survives a dedupe no-op: clicking where you
  // already are is not a new navigation.
  Stack branched;
  if (!PRISM_CHECK(branched.Visit("a"))) return;
  if (!PRISM_CHECK(branched.Visit("b"))) return;
  if (!PRISM_CHECK(branched.Back(nullptr))) return;
  if (!PRISM_CHECK_EQ(branched.ForwardCount(), size_t{1})) return;
  const std::string here = branched.Current();
  if (!PRISM_CHECK(!branched.Visit(here))) return;
  if (!PRISM_CHECK_EQ(branched.ForwardCount(), size_t{1})) return;
}

PRISM_TEST("history: a new visit truncates the forward branch") {
  Stack stack;
  if (!PRISM_CHECK(stack.Visit("a"))) return;
  if (!PRISM_CHECK(stack.Visit("b"))) return;
  if (!PRISM_CHECK(stack.Back(nullptr))) return;  // now at "a", forward = [b]
  if (!PRISM_CHECK(stack.CanForward())) return;
  if (!PRISM_CHECK(stack.Visit("c"))) return;
  if (!PRISM_CHECK(!stack.CanForward())) return;
  if (!PRISM_CHECK_EQ(stack.Current(), std::string("c"))) return;
  if (!PRISM_CHECK_EQ(stack.BackCount(), size_t{2})) return;  // root, "a"
}

PRISM_TEST("history: capacity is exact, not capacity-1 (CONTRACTS D17)") {
  Stack stack(3);
  for (int i = 0; i < 10; ++i) {
    const std::string folder = "f" + std::to_string(i);
    if (!PRISM_CHECK(stack.Visit(folder))) return;
  }
  // Ten visits from the root produce eleven back entries; the promise is
  // three, and a stack that keeps only two would silently lose a level.
  if (!PRISM_CHECK_EQ(stack.BackCount(), size_t{3})) return;
  if (!PRISM_CHECK_EQ(stack.Current(), std::string("f9"))) return;

  std::string out;
  if (!PRISM_CHECK(stack.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("f8"))) return;
  if (!PRISM_CHECK(stack.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("f7"))) return;
  if (!PRISM_CHECK(stack.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("f6"))) return;
  if (!PRISM_CHECK(!stack.Back(&out))) return;  // oldest entry was dropped
  if (!PRISM_CHECK_EQ(stack.ForwardCount(), size_t{3})) return;

  Stack tiny(1);
  if (!PRISM_CHECK(tiny.Visit("a"))) return;
  if (!PRISM_CHECK(tiny.Visit("b"))) return;
  if (!PRISM_CHECK(tiny.Visit("c"))) return;
  if (!PRISM_CHECK_EQ(tiny.BackCount(), size_t{1})) return;
  if (!PRISM_CHECK(tiny.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("b"))) return;
  if (!PRISM_CHECK(!tiny.Back(&out))) return;
  // A zero capacity is coerced to one: "history disabled" is expressed by not
  // recording visits, never by a container that promises nothing.
  Stack zero(0);
  if (!PRISM_CHECK_EQ(zero.capacity(), size_t{1})) return;
}

PRISM_TEST("history: folders are normalized before they are stored") {
  Stack stack;
  if (!PRISM_CHECK(stack.Visit("/docs//manual/"))) return;
  if (!PRISM_CHECK_EQ(stack.Current(), std::string("docs/manual"))) return;
  if (!PRISM_CHECK(!stack.Visit("docs\\manual"))) return;  // same level, de-duped
  if (!PRISM_CHECK_EQ(stack.Visit("docs\\tools"), true)) return;
  if (!PRISM_CHECK_EQ(stack.Current(), std::string("docs/tools"))) return;
  // "/" IS the archive root, so visiting it from a folder is a real move.
  if (!PRISM_CHECK(stack.Visit("/"))) return;
  if (!PRISM_CHECK_EQ(stack.Current(), std::string(""))) return;
}

PRISM_TEST("history: non-adjacent duplicates are real history") {
  Stack stack;
  if (!PRISM_CHECK(stack.Visit("a"))) return;
  if (!PRISM_CHECK(stack.Visit("b"))) return;
  if (!PRISM_CHECK(stack.Visit("a"))) return;  // legitimately back to a
  if (!PRISM_CHECK_EQ(stack.BackCount(), size_t{3})) return;  // root, a, b
  std::string out;
  if (!PRISM_CHECK(stack.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("b"))) return;
  if (!PRISM_CHECK(stack.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string("a"))) return;
  if (!PRISM_CHECK(stack.Back(&out))) return;
  if (!PRISM_CHECK_EQ(out, std::string(""))) return;
}

PRISM_TEST("history: Clear forgets everything") {
  Stack stack;
  if (!PRISM_CHECK(stack.Visit("a"))) return;
  if (!PRISM_CHECK(stack.Visit("b"))) return;
  stack.Clear();
  if (!PRISM_CHECK_EQ(stack.Current(), std::string(""))) return;
  if (!PRISM_CHECK(!stack.CanBack())) return;
  if (!PRISM_CHECK(!stack.CanForward())) return;
}

PRISM_TEST_MAIN()
