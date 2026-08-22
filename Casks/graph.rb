# Homebrew cask for macOS. Installs Graph.app into /Applications — so it is
# in Spotlight, Launchpad and the Dock — and links the graph command it
# carries into PATH. The wrklabs publisher fills in the version and the
# sha256 of the release tarball it mirrors to brew.wrklabs.org, and commits
# the result to the tap.
cask "graph" do
  version "__VERSION__"
  sha256 "__SHA_ARM64__"

  url "https://brew.wrklabs.org/dist/graph/graph-#{version}-darwin-arm64.tar.gz"
  name "graph"
  desc "Personal knowledge hub"
  homepage "https://wrklabs.org/graph"

  depends_on macos: :monterey
  depends_on arch: :arm64

  app "Graph.app"
  binary "#{appdir}/Graph.app/Contents/MacOS/graph"

  # The app is built by CI, not yet signed with a Developer ID. Clearing the
  # quarantine flag lets it open like a locally built one would; drop this
  # once releases are notarized.
  postflight do
    system_command "/usr/bin/xattr",
                   args: ["-cr", "#{appdir}/Graph.app"],
                   sudo: false
  end

  zap trash: "~/Library/Application Support/graph"
end
