/**
 * File: news-aggregator.cc
 * --------------------------------
 * Presents the implementation of the NewsAggregator class.
 */

#include "news-aggregator.h"
#include <iostream>
#include <iomanip>
#include <getopt.h>
#include <libxml/parser.h>
#include <libxml/catalog.h>
// you will almost certainly need to add more system header includes
#include <thread>
// I'm not giving away too much detail here by leaking the #includes below,
// which contribute to the official CS110 staff solution.
#include "rss-feed.h"
#include "rss-feed-list.h"
#include "html-document.h"
#include "html-document-exception.h"
#include "rss-feed-exception.h"
#include "rss-feed-list-exception.h"
#include "utils.h"
#include "ostreamlock.h"
#include "string-utils.h"
using namespace std;

/**
 * Factory Method: createNewsAggregator
 * ------------------------------------
 * Factory method that spends most of its energy parsing the argument vector
 * to decide what rss feed list to process and whether to print lots of
 * of logging information as it does so.
 */
static const string kDefaultRSSFeedListURL = "small-feed.xml";
NewsAggregator *NewsAggregator::createNewsAggregator(int argc, char *argv[]) {
  struct option options[] = {
    {"verbose", no_argument, NULL, 'v'},
    {"quiet", no_argument, NULL, 'q'},
    {"url", required_argument, NULL, 'u'},
    {NULL, 0, NULL, 0},
  };
  
  string rssFeedListURI = kDefaultRSSFeedListURL;
  bool verbose = false;
  while (true) {
    int ch = getopt_long(argc, argv, "vqu:", options, NULL);
    if (ch == -1) break;
    switch (ch) {
    case 'v':
      verbose = true;
      break;
    case 'q':
      verbose = false;
      break;
    case 'u':
      rssFeedListURI = optarg;
      break;
    default:
      NewsAggregatorLog::printUsage("Unrecognized flag.", argv[0]);
    }
  }
  
  argc -= optind;
  if (argc > 0) NewsAggregatorLog::printUsage("Too many arguments.", argv[0]);
  return new NewsAggregator(rssFeedListURI, verbose);
}

/**
 * Method: buildIndex
 * ------------------
 * Initalizex the XML parser, processes all feeds, and then
 * cleans up the parser.  The lion's share of the work is passed
 * on to processAllFeeds, which you will need to implement.
 */
void NewsAggregator::buildIndex() {
  if (built) return;
  built = true; // optimistically assume it'll all work out
  xmlInitParser();
  xmlInitializeCatalog();
  processAllFeeds();
  xmlCatalogCleanup();
  xmlCleanupParser();
}

/**
 * Method: queryIndex
 * ------------------
 * Interacts with the user via a custom command line, allowing
 * the user to surface all of the news articles that contains a particular
 * search term.
 */
void NewsAggregator::queryIndex() const {
  static const size_t kMaxMatchesToShow = 15;
  while (true) {
    cout << "Enter a search term [or just hit <enter> to quit]: ";
    string response;
    getline(cin, response);
    response = trim(response);
    if (response.empty()) break;
    const vector<pair<Article, int> >& matches = index.getMatchingArticles(response);
    if (matches.empty()) {
      cout << "Ah, we didn't find the term \"" << response << "\". Try again." << endl;
    } else {
      cout << "That term appears in " << matches.size() << " article"
           << (matches.size() == 1 ? "" : "s") << ".  ";
      if (matches.size() > kMaxMatchesToShow)
        cout << "Here are the top " << kMaxMatchesToShow << " of them:" << endl;
      else if (matches.size() > 1)
        cout << "Here they are:" << endl;
      else
        cout << "Here it is:" << endl;
      size_t count = 0;
      for (const pair<Article, int>& match: matches) {
        if (count == kMaxMatchesToShow) break;
        count++;
        string title = match.first.title;
        if (shouldTruncate(title)) title = truncate(title);
        string url = match.first.url;
        if (shouldTruncate(url)) url = truncate(url);
        string times = match.second == 1 ? "time" : "times";
        cout << "  " << setw(2) << setfill(' ') << count << ".) "
             << "\"" << title << "\" [appears " << match.second << " " << times << "]." << endl;
        cout << "       \"" << url << "\"" << endl;
      }
    }
  }
}

/**
 * Private Constructor: NewsAggregator
 * -----------------------------------
 * Self-explanatory.  You may need to add a few lines of code to
 * initialize any additional fields you add to the private section
 * of the class definition.
 */
NewsAggregator::NewsAggregator(const string& rssFeedListURI, bool verbose): 
  log(verbose), rssFeedListURI(rssFeedListURI), built(false) {}

/**
 * Private Method: processAllFeeds
 * -------------------------------
 * Downloads and parses the encapsulated RSSFeedList, which itself
 * leads to RSSFeeds, which themsleves lead to HTMLDocuemnts, which
 * can be collectively parsed for their tokens to build a huge RSSIndex.
 * 
 * The vast majority of your Assignment 5 work has you implement this
 * method using multithreading while respecting the imposed constraints
 * outlined in the spec.
 */

void NewsAggregator::processAllFeeds() {
  try {
    RSSFeedList feeder(rssFeedListURI);
    feeder.parse();
    const auto& feeds = feeder.getFeeds();
    processFeeds(feeds);
    log.noteFullRSSFeedListDownloadEnd();
  } catch (RSSFeedListException& rfle) {
    log.noteFullRSSFeedListDownloadFailureAndExit(rssFeedListURI);
  }
}

void NewsAggregator::processFeeds(const map<string, string>& feeds) {
  vector<thread> threads;

  for (auto it = feeds.cbegin(); it != feeds.cend(); it++) {
    feedSem.wait();
    threads.push_back(thread([this](semaphore& s, mutex& feedUriLock, pair<string, string> feedPair){
      s.signal(on_thread_exit);
      const string& feedUri = feedPair.first, feedTitle = feedPair.second;

      feedUriLock.lock();
      if (seenFeedsUri.find(feedUri) != seenFeedsUri.end()) {
        feedUriLock.unlock();
        log.noteSingleFeedDownloadSkipped(feedUri);
        return;
      }
      seenFeedsUri.insert(feedUri);
      feedUriLock.unlock();

      try {
        RSSFeed feed(feedUri);
        log.noteSingleFeedDownloadBeginning(feedUri);
        feed.parse();
        processArticles(feed.getArticles());
        log.noteSingleFeedDownloadEnd(feedUri);
      } catch (RSSFeedException& rfe) {
        log.noteSingleFeedDownloadFailure(feedUri);
      }
    }, ref(feedSem), ref(feedUriLock), *it));
  }

  for (thread& t: threads) t.join();

  for (auto it = seenServerTitleToArticleTokens.cbegin(); it != seenServerTitleToArticleTokens.cend(); it++) {
    for (auto itt = it->second.cbegin(); itt != it->second.cend(); itt++) {
      index.add(itt->second.first, itt->second.second);
    }
  }
}

void NewsAggregator::processArticles(const vector<Article>& articles) {
  vector<thread> threads;

  for (auto& article: articles) {
    articleSem.wait();
    threads.push_back(thread([this](semaphore& articleSem, mutex& serverLock, Article article){
      articleSem.signal(on_thread_exit);

      articleUriLock.lock();
      if (seenArticlesUri.find(article.url) != seenArticlesUri.end()) {
        articleUriLock.unlock();
        log.noteSingleArticleDownloadSkipped(article);
        return;
      }
      seenArticlesUri.insert(article.url);
      articleUriLock.unlock();

      string server = getURLServer(article.url);
      try {
        serverSemLock.lock();
        unique_ptr<semaphore>& myServerSem = serverSem[server];
        if (myServerSem == nullptr) myServerSem.reset(new semaphore(8));
        serverSemLock.unlock();

        myServerSem->wait();
        HTMLDocument htmlDoc(article.url);
        log.noteSingleArticleDownloadBeginning(article);
        htmlDoc.parse();
//        log.noteSingleArticleDownloadFinished(article);
        myServerSem->signal();

        vector<string> tokens = htmlDoc.getTokens();
        sort(tokens.begin(), tokens.end());
        vector<string> newTokens;
        Article newArticle = article;

        serverLock.lock();
        const auto& serverIt = seenServerTitleToArticleTokens.find(server);
        bool possibleDupe = ((serverIt != seenServerTitleToArticleTokens.end())
                             && (serverIt->second.find(article.title) != serverIt->second.end()));

        if (possibleDupe) {
          const Article oldArticle = seenServerTitleToArticleTokens[server][article.title].first;
          const vector<string> oldTokens = seenServerTitleToArticleTokens[server][article.title].second;
          set_intersection(oldTokens.cbegin(), oldTokens.cend(), tokens.cbegin(), tokens.cend(), back_inserter(newTokens));
          newArticle = min(oldArticle, article);
        } else {
          // TODO: Is this creating a copy?
          newTokens = tokens;
        }
        seenServerTitleToArticleTokens[server][article.title] = {newArticle, newTokens};
        serverLock.unlock();
      } catch (HTMLDocumentException& hde) {
        log.noteSingleArticleDownloadFailure(article);
      }


    }, ref(articleSem), ref(serverLock), article));
  }

  for (thread& t: threads) t.join();
}
