#pragma once

#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace snaplite::toolbaricons {

inline bool ExactText(LPCWSTR text, int count, LPCWSTR expected) {
    if (!text || !expected) return false;
    const int expectedLength = static_cast<int>(wcslen(expected));
    if (count < 0) return wcscmp(text, expected) == 0;
    return count == expectedLength && wcsncmp(text, expected, expectedLength) == 0;
}

inline Gdiplus::Color CurrentColor(HDC dc) {
    const COLORREF c = GetTextColor(dc);
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline Gdiplus::RectF IconBox(const RECT& r, float size = 19.0f) {
    const float cx = (static_cast<float>(r.left) + static_cast<float>(r.right)) * 0.5f;
    const float cy = (static_cast<float>(r.top) + static_cast<float>(r.bottom)) * 0.5f;
    return {cx - size * 0.5f, cy - size * 0.5f, size, size};
}

inline void DrawArrowHead(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                          Gdiplus::PointF tip, Gdiplus::PointF tail, float size = 5.0f) {
    const float angle = std::atan2(tip.Y - tail.Y, tip.X - tail.X);
    constexpr float spread = 0.62f;
    Gdiplus::PointF a{
        tip.X - size * std::cos(angle - spread),
        tip.Y - size * std::sin(angle - spread)};
    Gdiplus::PointF b{
        tip.X - size * std::cos(angle + spread),
        tip.Y - size * std::sin(angle + spread)};
    g.DrawLine(&pen, tip, a);
    g.DrawLine(&pen, tip, b);
}

inline void DrawCursor(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.7f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::PointF pts[] = {
        {b.X + 4.0f, b.Y + 2.0f},
        {b.X + 4.0f, b.Y + 15.0f},
        {b.X + 7.8f, b.Y + 11.6f},
        {b.X + 11.0f, b.Y + 17.0f},
        {b.X + 13.8f, b.Y + 15.4f},
        {b.X + 10.6f, b.Y + 10.2f},
        {b.X + 15.5f, b.Y + 9.8f},
        {b.X + 4.0f, b.Y + 2.0f},
    };
    g.DrawLines(&pen, pts, static_cast<INT>(std::size(pts)));
}

inline void DrawShapeCategory(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.6f);
    g.DrawRectangle(&pen, b.X + 2.0f, b.Y + 5.0f, 10.0f, 9.0f);
    g.DrawEllipse(&pen, b.X + 8.0f, b.Y + 2.0f, 9.0f, 9.0f);
}

inline void DrawArrowCategory(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.7f);
    Gdiplus::PointF from{b.X + 2.0f, b.Y + 15.0f};
    Gdiplus::PointF to{b.X + 16.0f, b.Y + 3.0f};
    g.DrawLine(&pen, from, to);
    DrawArrowHead(g, pen, to, from, 5.0f);
}

inline void DrawPenIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.7f);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    g.DrawLine(&pen, b.X + 3.0f, b.Y + 15.0f, b.X + 14.5f, b.Y + 3.5f);
    g.DrawLine(&pen, b.X + 2.5f, b.Y + 16.0f, b.X + 6.0f, b.Y + 15.0f);
    g.DrawLine(&pen, b.X + 13.0f, b.Y + 2.5f, b.X + 16.5f, b.Y + 6.0f);
}

inline void DrawMosaicIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::SolidBrush brush(c);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float size = ((row + col) % 2 == 0) ? 4.0f : 3.0f;
            g.FillRectangle(&brush, b.X + 2.0f + col * 5.5f, b.Y + 2.0f + row * 5.5f, size, size);
        }
    }
}

inline void DrawTextIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.8f);
    g.DrawLine(&pen, b.X + 3.0f, b.Y + 3.0f, b.X + 16.0f, b.Y + 3.0f);
    g.DrawLine(&pen, b.X + 9.5f, b.Y + 3.0f, b.X + 9.5f, b.Y + 16.0f);
    g.DrawLine(&pen, b.X + 6.0f, b.Y + 16.0f, b.X + 13.0f, b.Y + 16.0f);
}

inline void DrawPinIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.6f);
    Gdiplus::PointF pts[] = {
        {b.X + 6.0f, b.Y + 3.0f}, {b.X + 14.0f, b.Y + 3.0f},
        {b.X + 12.0f, b.Y + 8.0f}, {b.X + 15.0f, b.Y + 11.0f},
        {b.X + 5.0f, b.Y + 11.0f}, {b.X + 8.0f, b.Y + 8.0f},
        {b.X + 6.0f, b.Y + 3.0f},
    };
    g.DrawLines(&pen, pts, static_cast<INT>(std::size(pts)));
    g.DrawLine(&pen, b.X + 10.0f, b.Y + 11.0f, b.X + 10.0f, b.Y + 17.0f);
}

inline void DrawUndoRedo(Gdiplus::Graphics& g, const Gdiplus::RectF& b,
                         const Gdiplus::Color& c, bool redo) {
    Gdiplus::Pen pen(c, 1.7f);
    const float x = b.X + 2.0f;
    const float y = b.Y + 3.0f;
    g.DrawArc(&pen, x, y, 15.0f, 13.0f, redo ? 205.0f : -25.0f, redo ? 250.0f : -250.0f);
    Gdiplus::PointF tip = redo ? Gdiplus::PointF{b.X + 16.0f, b.Y + 7.0f}
                               : Gdiplus::PointF{b.X + 3.0f, b.Y + 7.0f};
    Gdiplus::PointF tail = redo ? Gdiplus::PointF{b.X + 12.0f, b.Y + 3.0f}
                                : Gdiplus::PointF{b.X + 7.0f, b.Y + 3.0f};
    DrawArrowHead(g, pen, tip, tail, 4.0f);
}

inline void DrawCopyIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.5f);
    g.DrawRectangle(&pen, b.X + 3.0f, b.Y + 3.0f, 10.0f, 10.0f);
    g.DrawRectangle(&pen, b.X + 7.0f, b.Y + 7.0f, 10.0f, 10.0f);
}

inline void DrawSaveIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& b,
                         const Gdiplus::Color& c, bool saveAs) {
    Gdiplus::Pen pen(c, 1.5f);
    g.DrawRectangle(&pen, b.X + 3.0f, b.Y + 2.0f, 13.0f, 15.0f);
    g.DrawRectangle(&pen, b.X + 6.0f, b.Y + 3.0f, 7.0f, 4.0f);
    g.DrawRectangle(&pen, b.X + 6.0f, b.Y + 11.0f, 7.0f, 5.0f);
    if (saveAs) {
        Gdiplus::Pen accent(c, 2.0f);
        g.DrawLine(&accent, b.X + 12.5f, b.Y + 15.5f, b.X + 17.0f, b.Y + 11.0f);
    }
}

inline void DrawCancelIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& b, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.8f);
    g.DrawLine(&pen, b.X + 4.0f, b.Y + 4.0f, b.X + 15.0f, b.Y + 15.0f);
    g.DrawLine(&pen, b.X + 15.0f, b.Y + 4.0f, b.X + 4.0f, b.Y + 15.0f);
}

inline void DrawSpecificShape(Gdiplus::Graphics& g, const Gdiplus::RectF& b,
                              const Gdiplus::Color& c, int kind) {
    Gdiplus::Pen pen(c, 1.55f);
    const float l = b.X + 2.0f, t = b.Y + 3.0f, r = b.X + b.Width - 2.0f, d = b.Y + b.Height - 3.0f;
    const float cx = (l + r) * 0.5f, cy = (t + d) * 0.5f;
    switch (kind) {
    case 0: g.DrawRectangle(&pen, l, t + 1.0f, r-l, d-t-2.0f); break;
    case 1: {
        Gdiplus::GraphicsPath p;
        const float radius = 3.5f, diam = radius * 2.0f;
        p.AddArc(l, t, diam, diam, 180.0f, 90.0f);
        p.AddArc(r-diam, t, diam, diam, 270.0f, 90.0f);
        p.AddArc(r-diam, d-diam, diam, diam, 0.0f, 90.0f);
        p.AddArc(l, d-diam, diam, diam, 90.0f, 90.0f);
        p.CloseFigure(); g.DrawPath(&pen, &p); break;
    }
    case 2: {
        const float side = std::min(r-l, d-t);
        g.DrawEllipse(&pen, cx-side/2.0f, cy-side/2.0f, side, side); break;
    }
    case 3: g.DrawEllipse(&pen, l, t+2.0f, r-l, d-t-4.0f); break;
    case 4: g.DrawLine(&pen, l, d-1.0f, r, t+1.0f); break;
    case 5: {
        Gdiplus::PointF pts[] = {{cx,t},{r,d},{l,d}}; g.DrawPolygon(&pen, pts, 3); break;
    }
    case 6: {
        Gdiplus::PointF pts[] = {{cx,t},{r,cy},{cx,d},{l,cy}}; g.DrawPolygon(&pen, pts, 4); break;
    }
    default: {
        const float q=(r-l)*0.25f;
        Gdiplus::PointF pts[]={{l+q,t},{r-q,t},{r,cy},{r-q,d},{l+q,d},{l,cy}};
        g.DrawPolygon(&pen,pts,6); break;
    }
    }
}

inline void DrawFillMode(Gdiplus::Graphics& g, const Gdiplus::RectF& b,
                         const Gdiplus::Color& c, int mode) {
    Gdiplus::Pen pen(c, 1.4f);
    Gdiplus::SolidBrush brush(c);
    const float x=b.X+4.0f, y=b.Y+4.0f, s=11.0f;
    if (mode == 0) g.DrawRectangle(&pen,x,y,s,s);
    else if (mode == 1) g.FillRectangle(&brush,x,y,s,s);
    else { g.FillRectangle(&brush,x,y+s/2.0f,s,s/2.0f); g.DrawRectangle(&pen,x,y,s,s); }
}

inline void DrawArrowStyle(Gdiplus::Graphics& g, const Gdiplus::RectF& b,
                           const Gdiplus::Color& c, int kind) {
    const float width = kind == 1 ? 1.0f : kind == 2 ? 3.0f : 1.6f;
    Gdiplus::Pen pen(c, width);
    Gdiplus::PointF from{b.X+2.0f,b.Y+b.Height-4.0f};
    Gdiplus::PointF to{b.X+b.Width-3.0f,b.Y+4.0f};
    if (kind == 4) {
        Gdiplus::GraphicsPath path;
        path.AddBezier(from, {b.X+6.0f,b.Y+1.0f}, {b.X+b.Width-8.0f,b.Y+b.Height-2.0f}, to);
        g.DrawPath(&pen,&path);
        DrawArrowHead(g,pen,to,{b.X+b.Width-8.0f,b.Y+b.Height-2.0f},4.5f);
        return;
    }
    if (kind == 5 || kind == 6) {
        Gdiplus::PointF pts[] = {
            from,
            {kind==5 ? (from.X+to.X)*0.5f : from.X+(to.X-from.X)/3.0f, from.Y},
            {kind==5 ? (from.X+to.X)*0.5f : from.X+(to.X-from.X)/3.0f, to.Y},
            to};
        g.DrawLines(&pen,pts,4);
        DrawArrowHead(g,pen,to,pts[2],4.5f);
        return;
    }
    g.DrawLine(&pen,from,to);
    DrawArrowHead(g,pen,to,from,kind==2 ? 5.5f : 4.5f);
    if (kind == 3) DrawArrowHead(g,pen,from,to,4.5f);
}

inline void DrawStrokeSample(Gdiplus::Graphics& g, const Gdiplus::RectF& b,
                             const Gdiplus::Color& c, float width) {
    Gdiplus::Pen pen(c, width);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    const float y = b.Y + b.Height * 0.5f;
    g.DrawLine(&pen, b.X + 4.0f, y, b.X + b.Width - 4.0f, y);
}

inline int DrawTextOrIcon(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format) {
    if (!dc || !text || !rect || !(format & DT_CENTER) || !(format & DT_VCENTER)) {
        return ::DrawTextW(dc, text, count, rect, format);
    }

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::Color color = CurrentColor(dc);
    const Gdiplus::RectF box = IconBox(*rect);

    if (ExactText(text,count,L"选择")) DrawCursor(g,box,color);
    else if (ExactText(text,count,L"形状")) DrawShapeCategory(g,box,color);
    else if (ExactText(text,count,L"箭头")) DrawArrowCategory(g,box,color);
    else if (ExactText(text,count,L"画笔")) DrawPenIcon(g,box,color);
    else if (ExactText(text,count,L"马赛克")) DrawMosaicIcon(g,box,color);
    else if (ExactText(text,count,L"文字")) DrawTextIcon(g,box,color);
    else if (ExactText(text,count,L"图钉")) DrawPinIcon(g,box,color);
    else if (ExactText(text,count,L"撤销")) DrawUndoRedo(g,box,color,false);
    else if (ExactText(text,count,L"重做")) DrawUndoRedo(g,box,color,true);
    else if (ExactText(text,count,L"复制")) DrawCopyIcon(g,box,color);
    else if (ExactText(text,count,L"保存")) DrawSaveIcon(g,box,color,false);
    else if (ExactText(text,count,L"另存为")) DrawSaveIcon(g,box,color,true);
    else if (ExactText(text,count,L"取消")) DrawCancelIcon(g,box,color);
    else if (ExactText(text,count,L"矩形")) DrawSpecificShape(g,box,color,0);
    else if (ExactText(text,count,L"圆角")) DrawSpecificShape(g,box,color,1);
    else if (ExactText(text,count,L"圆形")) DrawSpecificShape(g,box,color,2);
    else if (ExactText(text,count,L"椭圆")) DrawSpecificShape(g,box,color,3);
    else if (ExactText(text,count,L"直线")) DrawSpecificShape(g,box,color,4);
    else if (ExactText(text,count,L"三角")) DrawSpecificShape(g,box,color,5);
    else if (ExactText(text,count,L"菱形")) DrawSpecificShape(g,box,color,6);
    else if (ExactText(text,count,L"六边")) DrawSpecificShape(g,box,color,7);
    else if (ExactText(text,count,L"描边")) DrawFillMode(g,box,color,0);
    else if (ExactText(text,count,L"填充")) DrawFillMode(g,box,color,1);
    else if (ExactText(text,count,L"两者")) DrawFillMode(g,box,color,2);
    else if (ExactText(text,count,L"直箭头")) DrawArrowStyle(g,box,color,0);
    else if (ExactText(text,count,L"细箭头")) DrawArrowStyle(g,box,color,1);
    else if (ExactText(text,count,L"粗箭头")) DrawArrowStyle(g,box,color,2);
    else if (ExactText(text,count,L"双向")) DrawArrowStyle(g,box,color,3);
    else if (ExactText(text,count,L"弯曲")) DrawArrowStyle(g,box,color,4);
    else if (ExactText(text,count,L"折线")) DrawArrowStyle(g,box,color,5);
    else if (ExactText(text,count,L"阶梯")) DrawArrowStyle(g,box,color,6);
    else if (ExactText(text,count,L"1") || ExactText(text,count,L"细")) DrawStrokeSample(g,box,color,1.0f);
    else if (ExactText(text,count,L"2")) DrawStrokeSample(g,box,color,2.0f);
    else if (ExactText(text,count,L"普通")) DrawStrokeSample(g,box,color,3.0f);
    else if (ExactText(text,count,L"4")) DrawStrokeSample(g,box,color,4.0f);
    else if (ExactText(text,count,L"粗")) DrawStrokeSample(g,box,color,5.5f);
    else if (ExactText(text,count,L"6")) DrawStrokeSample(g,box,color,6.0f);
    else if (ExactText(text,count,L"很粗")) DrawStrokeSample(g,box,color,8.0f);
    else if (ExactText(text,count,L"更多")) {
        Gdiplus::SolidBrush brush(color);
        const float cy = box.Y + box.Height * 0.5f;
        for (int i=0;i<3;++i) g.FillEllipse(&brush, box.X+4.0f+i*5.0f, cy-1.5f, 3.0f, 3.0f);
    } else {
        return ::DrawTextW(dc, text, count, rect, format);
    }
    return rect->bottom - rect->top;
}

}  // namespace snaplite::toolbaricons

#define DrawTextW(...) snaplite::toolbaricons::DrawTextOrIcon(__VA_ARGS__)
