#include "check.h"

#include <string>

#include "animelogon/skin.h"
#include "animelogon/xml.h"

using namespace animelogon;

namespace {

bool ParsesXml(const char *text, std::wstring *error = nullptr) {
    xml::Element root;
    std::wstring e;
    const bool ok = xml::Parse(text, &root, &e);
    if (error) *error = e;
    return ok;
}

std::string Wrap(const std::string &panel, const std::string &settings = {}) {
    return "<skin format=\"1\" name=\"t\">" + settings + panel + "</skin>";
}

bool ParsesSkin(const std::string &text, std::wstring *error = nullptr) {
    skin::Skin s;
    return skin::Parse(text, &s, error);
}

}  // namespace

TEST(XmlElementsAndAttributes) {
    xml::Element root;
    std::wstring error;
    CHECK(xml::Parse("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- c --><a x='1' y=\"&lt;&#x41;&#66;\">\n <b/><c></c>\n</a>\n",
                     &root, &error));
    CHECK(root.name == L"a" && root.children.size() == 2 && root.children[1].name == L"c");
    CHECK(root.Find(L"x") && *root.Find(L"x") == L"1");
    CHECK(root.Find(L"y") && *root.Find(L"y") == L"<AB");
    CHECK(root.line == 2 && root.children[0].line == 3);
}

TEST(XmlRefusesWhatSkinsDoNotUse) {
    std::wstring error;
    CHECK(!ParsesXml("<!DOCTYPE a [<!ENTITY e \"x\">]><a/>"));
    CHECK(!ParsesXml("<a><![CDATA[x]]></a>"));
    CHECK(!ParsesXml("<a>text</a>"));
    CHECK(!ParsesXml("<a x=\"&e;\"/>"));
    CHECK(!ParsesXml("<a x=\"&#0;\"/>"));
    CHECK(!ParsesXml("<a x=\"&#xD800;\"/>"));
    CHECK(!ParsesXml("<a x=\"1\" x=\"2\"/>"));
    CHECK(!ParsesXml("<a x=\"<\"/>"));
    CHECK(!ParsesXml("<a><b></a></b>"));
    CHECK(!ParsesXml("<a/><b/>"));
    CHECK(!ParsesXml("<a><?pi x?></a>"));
    CHECK(!ParsesXml("<a x=1/>"));
    CHECK(!ParsesXml("<a\x01/>"));
    CHECK(!ParsesXml("<a x=\"\xC3\"/>"));  // not UTF-8
    CHECK(!ParsesXml("<a><b><c><d><e/></d></c></b></a>", &error));
    CHECK(error.find(L"line 1") != std::wstring::npos);
    std::string many = "<a>";
    for (int i = 0; i < 300; ++i) many += "<b/>";
    CHECK(!ParsesXml((many + "</a>").c_str()));
    CHECK(!ParsesXml(("<a x=\"" + std::string(600, 'x') + "\"/>").c_str()));
}

TEST(XmlWritesWhatItReads) {
    xml::Element root, again;
    CHECK(xml::Parse("<a q='&quot;&amp;&gt;'><b x=\"\xE6\x97\xB6\"/></a>", &root, nullptr));
    CHECK(xml::Parse(xml::Write(root), &again, nullptr));
    CHECK(again.Find(L"q") && *again.Find(L"q") == L"\"&>");
    CHECK(again.children.size() == 1 && *again.children[0].Find(L"x") == L"\x65F6");
}

TEST(SkinDefaultParses) {
    skin::Skin s;
    std::wstring error;
    CHECK(skin::Parse(skin::DefaultText(), &s, &error));
    CHECK(error.empty());
    CHECK(s.name == L"\x65F6\x949F");  // 时钟
    CHECK(s.Find(L"weight") && s.Find(L"weight")->fallback == L"semibold");
    CHECK(s.Find(L"date") && s.Find(L"date")->fallback == L"{date}");
    skin::Skin again;
    CHECK(skin::Parse(skin::Normalize(s), &again, nullptr) && again.settings.size() == s.settings.size());
}

TEST(SkinResolvesSettings) {
    const skin::Skin &s = skin::Default();
    skin::Resolved r = skin::Resolve(s, {});
    CHECK(r.panels.size() == 1);
    const skin::Panel &p = r.panels[0];
    CHECK(p.anchor == 1 && p.size == 10.0f && p.backdrop > 0.2f && p.hours == skin::Hours::Auto);
    CHECK(p.lines.size() == 2 && p.lines[0].texts.size() == 2);
    CHECK(p.lines[0].texts[0].value == L"{time}" && p.lines[0].texts[0].weight == 600);
    CHECK(p.lines[0].texts[1].value == L"{ampm}" && p.lines[0].texts[1].hang);
    CHECK(p.lines[1].texts[0].value == L"{date}");
    CHECK(p.shadows.size() == 2);

    r = skin::Resolve(s, {{L"position", L"bottom-left"}, {L"size", L"15"}, {L"weight", L"light"},
                          {L"color", L"#FFD27A"}, {L"shade", L"off"}, {L"ampm", L"off"}, {L"date", L""},
                          {L"hours", L"24"}, {L"language", L"ja-JP"}});
    const skin::Panel &q = r.panels[0];
    CHECK(q.anchor == 6 && q.size == 15.0f && q.backdrop == 0.0f && q.hours == skin::Hours::H24);
    CHECK(q.locale == L"ja-JP" && q.lines[0].texts[0].weight == 300);
    CHECK(q.lines[0].texts[0].color == 0xFFFFD27A);
    CHECK(q.lines[0].texts[1].value.empty() && q.lines[1].texts[0].value.empty());

    // Values that do not fit fall back to the defaults.
    r = skin::Resolve(s, {{L"size", L"99"}, {L"weight", L"heavy"}, {L"color", L"red"}, {L"font", L"a\\b"}});
    CHECK(r.panels[0].size == 10.0f && r.panels[0].lines[0].texts[0].weight == 600);
    CHECK(r.panels[0].lines[0].texts[0].color == 0xFFFFFFFF && r.panels[0].lines[0].texts[0].font.empty());
}

TEST(SkinRefusesBadStructure) {
    const std::string line = "<line><text value=\"{time}\"/></line>";
    CHECK(ParsesSkin(Wrap("<panel>" + line + "</panel>")));
    CHECK(!ParsesSkin("<skin format=\"2\" name=\"t\"><panel>" + line + "</panel></skin>"));
    CHECK(!ParsesSkin("<skin format=\"1\"><panel>" + line + "</panel></skin>"));
    CHECK(!ParsesSkin(Wrap("")));
    CHECK(!ParsesSkin(Wrap("<panel></panel>")));
    CHECK(!ParsesSkin(Wrap("<panel script=\"x\">" + line + "</panel>")));
    CHECK(!ParsesSkin(Wrap("<panel><line><text value=\"{password}\"/></line></panel>")));
    CHECK(!ParsesSkin(Wrap("<panel><line><text value=\"{time\"/></line></panel>")));
    CHECK(!ParsesSkin(Wrap("<panel><line><text value=\"}\"/></line></panel>")));
    CHECK(!ParsesSkin(Wrap("<panel size=\"80\">" + line + "</panel>")));
    CHECK(!ParsesSkin(Wrap("<panel><line><text value=\"x\" color=\"blue\"/></line></panel>")));
    CHECK(!ParsesSkin(Wrap("<panel><line><image/></line></panel>")));
    CHECK(!ParsesSkin(Wrap("<panel anchor=\"$where\">" + line + "</panel>")));
}

TEST(SkinChecksSettingsAgainstTheirUse) {
    const std::string line = "<line><text value=\"{time}\"/></line>";
    const std::string size = "<settings><number id=\"s\" label=\"S\" default=\"10\" min=\"5\" max=\"20\"/></settings>";
    CHECK(ParsesSkin(Wrap("<panel size=\"$s\">" + line + "</panel>", size)));
    // A number whose range exceeds what the attribute takes.
    const std::string wide = "<settings><number id=\"s\" label=\"S\" default=\"10\" min=\"5\" max=\"90\"/></settings>";
    CHECK(!ParsesSkin(Wrap("<panel size=\"$s\">" + line + "</panel>", wide)));
    // A colour setting cannot feed a size; a choice whose options do not all fit cannot either.
    const std::string color = "<settings><color id=\"c\" label=\"C\" default=\"#FFFFFF\"/></settings>";
    CHECK(!ParsesSkin(Wrap("<panel size=\"$c\">" + line + "</panel>", color)));
    const std::string choice =
        "<settings><choice id=\"a\" label=\"A\" default=\"top\"><option value=\"top\" label=\"T\"/>"
        "<option value=\"middle\" label=\"M\"/></choice></settings>";
    CHECK(!ParsesSkin(Wrap("<panel anchor=\"$a\">" + line + "</panel>", choice)));
    // Defaults must be values the setting can take; ids must be unique.
    CHECK(!ParsesSkin(Wrap("<panel>" + line + "</panel>",
                           "<settings><number id=\"s\" label=\"S\" default=\"30\" min=\"5\" max=\"20\"/></settings>")));
    CHECK(!ParsesSkin(Wrap("<panel>" + line + "</panel>",
                           "<settings><color id=\"c\" label=\"C\" default=\"#FFF\"/>"
                           "<color id=\"c\" label=\"D\" default=\"#FFFFFF\"/></settings>")));
    CHECK(!ParsesSkin(Wrap("<panel>" + line + "</panel>",
                           "<settings><toggle id=\"t\" label=\"T\" default=\"yes\" on=\"1\" off=\"0\"/></settings>")));
}

TEST(SkinSettingValues) {
    skin::Setting font;
    font.kind = skin::SettingKind::Font;
    CHECK(skin::IsValue(font, L"") && skin::IsValue(font, L"Segoe UI") && !skin::IsValue(font, L"a\\b"));
    skin::Setting color;
    color.kind = skin::SettingKind::Color;
    CHECK(skin::IsValue(color, L"#12AB34") && skin::IsValue(color, L"#12AB3480") && !skin::IsValue(color, L"#12AB3"));
    CHECK(skin::IsSkinId(L"default") && skin::IsSkinId(L"0123456789abcdef") && !skin::IsSkinId(L"../x"));
    CHECK(skin::IsSettingId(L"date-style") && !skin::IsSettingId(L"Date") && !skin::IsSettingId(L"a.b"));
}
