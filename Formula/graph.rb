class Graph < Formula
  desc "Filesystem repository shared over SMB"
  homepage "https://wrklabs.org/graph"
  url "https://github.com/wrk-labs/graph/archive/refs/tags/v__VERSION__.tar.gz"
  sha256 "__SHA__"
  version "__VERSION__"
  license "GPL-2.0-only"
  head "https://github.com/wrk-labs/graph.git", branch: "main"

  def install
    system "make"
    bin.install "graph"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/graph -v")
  end
end
