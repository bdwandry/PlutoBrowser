// internal_pages.c — locally served about: pages for the HTTP client.
// GENERATED from CometBrowser Source/core/http_client.lua INTERNAL_PAGES
// via p07_dump_pages.lua (byte-exact extraction); do not hand-edit.

#include "internal_pages.h"
#include <string.h>

static const char hc_page_home_html[] =
    "<html><head><title>CometBrowser</title></head><body><h1>CometBrowser</h1><p>Ready.</p></body></html>"
;
#define HC_PAGE_HOME_LEN (sizeof(hc_page_home_html) - 1)
static const char hc_page_home_title[] = "CometBrowser";

static const char hc_page_blank_html[] =
    "<html><body></body></html>"
;
#define HC_PAGE_BLANK_LEN (sizeof(hc_page_blank_html) - 1)
static const char hc_page_blank_title[] = "Blank";

static const char hc_page_acid_html[] =
    "<html><head><title>HTML Renderer Test Suite</title></head><body>\n"
    "\n"
    "<h1>HTML Renderer Test</h1>\n"
    "<p>Every block &amp; inline element the renderer understands, in one page. Some <b>bold</b>, <i>italic</i>, <u>underlined</u>, <s>struck</s>, <code>code</code>, <mark>marked</mark>, <small>small</small>, <big>big</big> and <sub>sub</sub>/<sup>sup</sup> text, plus an <a href=\"https://example.com\">example link</a> and a <q>short quote</q>.</p>\n"
    "\n"
    "<h2>Headings &amp; Alignment</h2>\n"
    "<h3>Left</h3>\n"
    "<div align=\"center\"><p>This paragraph is centered via align.</p></div>\n"
    "<p style=\"text-align:right\">This paragraph is right-aligned via inline style.</p>\n"
    "\n"
    "<h2>Lists</h2>\n"
    "<ul><li>Unordered item one</li><li>Item two with a nested list:<ul><li>Nested item A</li><li>Nested item B</li></ul></li><li>Item three</li></ul>\n"
    "<ol><li>First ordered</li><li>Second ordered</li><li>Third ordered</li></ol>\n"
    "<dl><dt>Definition term</dt><dd>Definition description that runs on for a bit so we can see wrapping work.</dd><dt>Another term</dt><dd>Another description.</dd></dl>\n"
    "\n"
    "<h2>Quotes &amp; Code</h2>\n"
    "<blockquote>This is a block quotation with a left rail, the way desktop browsers draw them.</blockquote>\n"
    "<pre>function hello()\n"
    "  print(\"Hello, Playdate\")\n"
    "end</pre>\n"
    "\n"
    "<h2>Tables</h2>\n"
    "<table>\n"
    "<caption>Sample Caption</caption>\n"
    "<thead><tr><th>Name</th><th>Score</th><th>Level</th></tr></thead>\n"
    "<tbody>\n"
    "<tr><td>Bryan</td><td align=\"right\">98</td><td>5</td></tr>\n"
    "<tr><td>Comet</td><td align=\"right\">87</td><td>4</td></tr>\n"
    "</tbody>\n"
    "</table>\n"
    "\n"
    "<h2>Figures</h2>\n"
    "<figure><img src=\"https://example.com/test.png\" width=\"160\" height=\"80\" alt=\"Alt text placeholder\"><figcaption>A figure with a caption</figcaption></figure>\n"
    "\n"
    "<h2>Forms</h2>\n"
    "<form action=\"https://example.com/search\" method=\"get\">\n"
    "<label>Search:</label> <input type=\"text\" name=\"q\" placeholder=\"type here\">\n"
    "<input type=\"submit\" value=\"Search\">\n"
    "<fieldset><legend>Preferences</legend>\n"
    "<input type=\"checkbox\" name=\"opt1\" checked> Option one (checked)<br>\n"
    "<input type=\"checkbox\" name=\"opt2\"> Option two<br>\n"
    "<input type=\"radio\" name=\"grp\" value=\"a\" checked> Radio A\n"
    "<input type=\"radio\" name=\"grp\" value=\"b\"> Radio B\n"
    "<select name=\"color\"><option selected>Red</option><option>Green</option><option>Blue</option></select>\n"
    "</fieldset>\n"
    "<textarea name=\"msg\" rows=\"2\">Hello textarea</textarea>\n"
    "</form>\n"
    "\n"
    "<h2>Boxes</h2>\n"
    "<details><summary>Clickable summary line</summary><p>Hidden-until-open body content is shown inline on Playdate.</p></details>\n"
    "<dialog open><p>A dialog box with an open attribute.</p></dialog>\n"
    "\n"
    "<h2>Media &amp; Meters</h2>\n"
    "<p>Progress: <progress value=\"70\" max=\"100\"></progress>  Meter: <meter value=\"0.6\" max=\"1\"></meter></p>\n"
    "<video controls width=\"300\" height=\"120\"><source src=\"movie.mp4\"></video>\n"
    "<iframe width=\"200\" height=\"100\"></iframe>\n"
    "\n"
    "<h2>Horizontal Rule &amp; Misc</h2>\n"
    "<hr>\n"
    "<p>Entities: &amp; &lt; &gt; &quot; &apos; &nbsp; &copy; &mdash; &hellip; 5 &lt; 6 &amp; 4 = 9</p>\n"
    "<p>Unicode fallbacks: &Auml; &ouml; &eacute; &nbsp;</p>\n"
    "\n"
    "</body></html>"
;
#define HC_PAGE_ACID_LEN (sizeof(hc_page_acid_html) - 1)
static const char hc_page_acid_title[] = "HTML Acid Test";

static const HcInternalPage HC_PAGES[] = {
    {"about:home", hc_page_home_title, hc_page_home_html, HC_PAGE_HOME_LEN},
    {"about:blank", hc_page_blank_title, hc_page_blank_html, HC_PAGE_BLANK_LEN},
    {"about:acidtest", hc_page_acid_title, hc_page_acid_html, HC_PAGE_ACID_LEN},
};

const HcInternalPage* hc_internal_page(const char* url)
{
    for (size_t i = 0; i < sizeof(HC_PAGES)/sizeof(HC_PAGES[0]); i++)
        if (strcmp(HC_PAGES[i].url, url) == 0) return &HC_PAGES[i];
    return NULL;
}
