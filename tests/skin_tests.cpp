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
    return "<component format=\"1\" name=\"t\">" + settings + panel + "</component>";
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
    CHECK(s.Find(L"size") && s.Find(L"size")->fallback == L"10");
    // What the clock says is the clock's, not the skin's.
    CHECK(!s.Find(L"date") && !s.Find(L"hours") && !s.Find(L"language") && !s.Find(L"ampm"));
    skin::Skin again;
    CHECK(skin::Parse(skin::Normalize(s), &again, nullptr) && again.settings.size() == s.settings.size());
}

TEST(SkinResolvesSettings) {
    const skin::Skin &s = skin::Default();
    skin::Resolved r = skin::Resolve(s, {});
    CHECK(r.panels.size() == 1);
    const skin::Panel &p = r.panels[0];
    CHECK(p.anchor == 1 && p.size == 10.0f && p.backdrop > 0.2f);
    CHECK(p.lines.size() == 2 && p.lines[0].texts.size() == 2);
    CHECK(p.lines[0].texts[0].value == L"{time}" && p.lines[0].texts[0].weight == 600);
    CHECK(p.lines[0].texts[1].value == L"{ampm}" && p.lines[0].texts[1].hang);
    CHECK(p.lines[1].texts[0].value == L"{date}");
    // No shadow by default; one is there, at nothing, to be turned up.
    CHECK(p.shadows.size() == 1 && p.shadows[0].opacity == 0.0f);

    r = skin::Resolve(s, {{L"position", L"bottom-left"}, {L"size", L"15"}, {L"color", L"#FFD27A"},
                          {L"shade", L"off"}});
    const skin::Panel &q = r.panels[0];
    CHECK(q.anchor == 6 && q.size == 15.0f && q.backdrop == 0.0f);
    CHECK(q.lines[0].texts[0].color == 0xFFFFD27A);

    // Values that do not fit fall back to the defaults.
    r = skin::Resolve(s, {{L"size", L"99"}, {L"color", L"red"}, {L"font", L"a\\b"}});
    CHECK(r.panels[0].size == 10.0f && r.panels[0].lines[0].texts[0].weight == 600);
    CHECK(r.panels[0].lines[0].texts[0].color == 0xFFFFFFFF && r.panels[0].lines[0].texts[0].font.empty());
}

TEST(SkinTakesAdjustments) {
    const skin::Skin &s = skin::Default();
    // By hand, element by element; over the skin's own values and its settings'.
    skin::Resolved r = skin::Resolve(s, {{L"size", L"15"},
                                         {L"p1.size", L"12"},
                                         {L"p1.offset-x", L"-3.5"},
                                         {L"p1.l1.t1.weight", L"350"},
                                         {L"p1.l1.t1.tracking", L"0.05"},
                                         {L"p1.l2.t1.color", L"#FF000080"},
                                         {L"p1.l2.gap", L"0.4"},
                                         {L"p1.s1.opacity", L"0.5"},
                                         {L"p1.b.opacity", L"0"}});
    const skin::Panel &p = r.panels[0];
    CHECK(p.size == 12.0f && p.offsetX == -3.5f && p.backdrop == 0.0f);
    CHECK(p.lines[0].texts[0].weight == 350 && p.lines[0].texts[0].tracking == 0.05f);
    CHECK(p.lines[1].texts[0].color == 0x80FF0000 && p.lines[1].gap == 0.4f);
    CHECK(p.shadows[0].opacity == 0.5f);
    // Out of range, of the wrong kind, or not adjustable: ignored.
    r = skin::Resolve(s, {{L"p1.size", L"99"}, {L"p1.l1.t1.weight", L"heavy"}, {L"p1.l1.t1.value", L"x"},
                          {L"p1.l1.t2.hang", L"false"}});
    CHECK(r.panels[0].size == 10.0f && r.panels[0].lines[0].texts[0].weight == 600);
    CHECK(r.panels[0].lines[0].texts[0].value == L"{time}" && r.panels[0].lines[0].texts[1].hang);

    CHECK(skin::IsAdjustmentKey(L"p1.l2.t1.weight") && skin::IsAdjustmentKey(L"p1.b.opacity"));
    CHECK(skin::IsAdjustmentKey(L"p2.s1.blur") && skin::IsAdjustmentKey(L"p1.offset-x"));
    CHECK(!skin::IsAdjustmentKey(L"p1.l1.t1.value") && !skin::IsAdjustmentKey(L"p1.l1.t1.label"));
    CHECK(!skin::IsAdjustmentKey(L"p0.size") && !skin::IsAdjustmentKey(L"p1.t1.size"));
    CHECK(!skin::IsAdjustmentKey(L"size") && !skin::IsAdjustmentKey(L"p1.l1.weight"));
    CHECK(skin::IsAdjustmentValue(L"p1.l1.t1.weight", L"350") && !skin::IsAdjustmentValue(L"p1.size", L"80"));
}

TEST(SkinOffersItsParts) {
    const skin::Skin &s = skin::Default();
    const std::vector<skin::Part> parts = skin::Parts(s);
    // The panel, time, AM/PM, date, shadow and backdrop.
    CHECK(parts.size() == 6);
    CHECK(parts[0].key == L"p1" && parts[1].key == L"p1.l1.t1" && parts[1].label == L"\x65F6\x95F4");  // 时间
    CHECK(parts[3].key == L"p1.l2.t1" && parts[4].key == L"p1.s1" && parts[5].key == L"p1.b");
    auto find = [](const skin::Part &part, const std::wstring &key) -> const skin::Adjustment * {
        for (const skin::Adjustment &a : part.adjustments)
            if (a.key == key) return &a;
        return nullptr;
    };
    // The date's line carries its distance from the line above; a first text has no space before it.
    CHECK(find(parts[3], L"p1.l2.gap") && !find(parts[1], L"p1.l1.gap") && !find(parts[1], L"p1.l1.t1.space"));
    CHECK(find(parts[2], L"p1.l1.t2.space"));
    const skin::Adjustment *weight = find(parts[1], L"p1.l1.t1.weight");
    CHECK(weight && skin::Effective(s, {}, *weight) == L"600");
    CHECK(skin::Effective(s, {{L"p1.l1.t1.weight", L"350"}}, *weight) == L"350");
    const skin::Adjustment *anchor = find(parts[0], L"p1.anchor");
    CHECK(anchor && skin::Effective(s, {{L"position", L"left"}}, *anchor) == L"left");
    const skin::Adjustment *offset = find(parts[0], L"p1.offset-x");
    CHECK(offset && skin::Effective(s, {}, *offset) == L"0");
    // Changing a setting brings back whatever takes its value from it.
    const std::vector<std::wstring> sized = skin::AdjustmentsOf(s, L"size");
    CHECK(sized.size() == 1 && sized[0] == L"p1.size");
    CHECK(skin::AdjustmentsOf(s, L"color").size() == 3);
}

TEST(SkinRefusesBadStructure) {
    const std::string line = "<line><text value=\"{time}\"/></line>";
    CHECK(ParsesSkin(Wrap("<panel>" + line + "</panel>")));
    CHECK(!ParsesSkin("<component format=\"2\" name=\"t\"><panel>" + line + "</panel></component>"));
    CHECK(!ParsesSkin("<component format=\"1\"><panel>" + line + "</panel></component>"));
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
    CHECK(skin::IsSettingId(L"date-style") && !skin::IsSettingId(L"Date") && !skin::IsSettingId(L"a.b"));
}
