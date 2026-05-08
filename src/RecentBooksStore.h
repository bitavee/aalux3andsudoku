#pragma once
#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  // Optional series metadata. Empty when the book has no calibre:series /
  // EPUB 3 belongs-to-collection. seriesIndex is kept as a string so values
  // like "1.5" round-trip exactly.
  std::string seriesName;
  std::string seriesIndex;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore;
namespace JsonSettingsIO {
bool loadRecentBooks(RecentBooksStore& store, const char* json);
}  // namespace JsonSettingsIO

class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<RecentBook> recentBooks;

  friend bool JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);

 public:
  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath, const std::string& seriesName = {},
               const std::string& seriesIndex = {});

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath, const std::string& seriesName = {},
                  const std::string& seriesIndex = {});

  // Remove the recents entry for `path`. No-op if the path is not in the
  // list. Persists to disk on success. Does not touch the book's cache
  // directory or stats — only the home recents list.
  bool removeBook(const std::string& path);

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  bool saveToFile() const;

  bool loadFromFile();
  RecentBook getDataFromBook(std::string path) const;

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
