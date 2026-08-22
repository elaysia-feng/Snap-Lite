#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace snaplite::toolbaricons_gdi {

inline bool Exact(LPCWSTR text, int count, LPCWSTR expected) {
    if (!text || !expected) return false;
    const int expectedLength = static_cast<int>(wcslen(expected));
    if (count < 0) return wcscmp(text, expected) == 0;
    return count == expectedLength && wcsncmp(text, expected, expectedLength) == 0;
}

inline POINT Center(const RECT& r) {
    return {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
}

inline HPEN MakePen(HDC dc, int width = 2) {
    return CreatePen(PS_SOLID, std::max(1, width), GetTextColor(dc));
}

inline void ArrowHead(HDC dc, POINT tip, POINT tail, int size = 5, int width = 2) {
    const double angle = std::atan2(static_cast<double>(tip.y - tail.y), static_cast<double>(tip.x - tail.x));
    constexpr double spread = 0.62;
    POINT a{
        static_cast<LONG>(tip.x - size * std::cos(angle - spread)),
        static_cast<LONG>(tip.y - size * std::sin(angle - spread))};
    POINT b{
        static_cast<LONG>(tip.x - size * std::cos(angle + spread)),
        static_cast<LONG>(tip.y - size * std::sin(angle + spread))};
    HPEN pen = MakePen(dc, width);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, tip.x, tip.y, nullptr); LineTo(dc, a.x, a.y);
    MoveToEx(dc, tip.x, tip.y, nullptr); LineTo(dc, b.x, b.y);
    SelectObject(dc, old); DeleteObject(pen);
}

inline void DrawCursor(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    POINT pts[] = {
        {c.x - 7, c.y - 8}, {c.x - 7, c.y + 6}, {c.x - 3, c.y + 2},
        {c.x + 1, c.y + 8}, {c.x + 4, c.y + 6}, {c.x, c.y},
        {c.x + 7, c.y}, {c.x - 7, c.y - 8}};
    HPEN pen = MakePen(dc, 2); HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, pts, static_cast<int>(sizeof(pts) / sizeof(pts[0])));
    SelectObject(dc, old); DeleteObject(pen);
}

inline void DrawShapeCategory(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    HPEN pen = MakePen(dc, 2); HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, c.x - 8, c.y - 4, c.x + 3, c.y + 7);
    Ellipse(dc, c.x - 1, c.y - 8, c.x + 9, c.y + 2);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

inline void DrawArrow(HDC dc, const RECT& r, int kind = 0) {
    const POINT c = Center(r);
    POINT from{c.x - 9, c.y + 6};
    POINT to{c.x + 9, c.y - 6};
    const int width = kind == 1 ? 1 : kind == 2 ? 4 : 2;
    HPEN pen = MakePen(dc, width); HGDIOBJ old = SelectObject(dc, pen);
    if (kind == 4) {
        POINT bezier[4] = {from, {c.x - 3, c.y - 9}, {c.x + 4, c.y + 5}, to};
        PolyBezier(dc, bezier, 4);
        SelectObject(dc, old); DeleteObject(pen);
        ArrowHead(dc, to, bezier[2], 5, width);
        return;
    }
    if (kind == 5 || kind == 6) {
        const LONG midx = kind == 5 ? c.x : from.x + (to.x - from.x) / 3;
        POINT pts[4] = {from, {midx, from.y}, {midx, to.y}, to};
        Polyline(dc, pts, 4);
        SelectObject(dc, old); DeleteObject(pen);
        ArrowHead(dc, to, pts[2], 5, width);
        return;
    }
    MoveToEx(dc, from.x, from.y, nullptr); LineTo(dc, to.x, to.y);
    SelectObject(dc, old); DeleteObject(pen);
    ArrowHead(dc, to, from, kind == 2 ? 6 : 5, width);
    if (kind == 3) ArrowHead(dc, from, to, 5, width);
}

inline void DrawPen(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    HPEN pen = MakePen(dc, 2); HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, c.x - 8, c.y + 7, nullptr); LineTo(dc, c.x + 6, c.y - 7);
    MoveToEx(dc, c.x - 9, c.y + 8, nullptr); LineTo(dc, c.x - 4, c.y + 7);
    MoveToEx(dc, c.x + 5, c.y - 8, nullptr); LineTo(dc, c.x + 9, c.y - 4);
    SelectObject(dc, old); DeleteObject(pen);
}

inline void DrawMosaic(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    HBRUSH brush = CreateSolidBrush(GetTextColor(dc));
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            RECT cell{c.x - 8 + col * 6, c.y - 8 + row * 6,
                      c.x - 4 + col * 6, c.y - 4 + row * 6};
            FillRect(dc, &cell, brush);
        }
    }
    DeleteObject(brush);
}

inline void DrawTextGlyph(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    HPEN pen = MakePen(dc, 2); HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, c.x - 7, c.y - 7, nullptr); LineTo(dc, c.x + 7, c.y - 7);
    MoveToEx(dc, c.x, c.y - 7, nullptr); LineTo(dc, c.x, c.y + 7);
    MoveToEx(dc, c.x - 4, c.y + 7, nullptr); LineTo(dc, c.x + 4, c.y + 7);
    SelectObject(dc, old); DeleteObject(pen);
}

inline void DrawPin(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    POINT pts[] = {{c.x - 5,c.y - 8},{c.x + 5,c.y - 8},{c.x + 3,c.y - 2},
                   {c.x + 6,c.y + 1},{c.x - 6,c.y + 1},{c.x - 3,c.y - 2},{c.x - 5,c.y - 8}};
    HPEN pen = MakePen(dc, 2); HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, pts, static_cast<int>(sizeof(pts) / sizeof(pts[0])));
    MoveToEx(dc, c.x, c.y + 1, nullptr); LineTo(dc, c.x, c.y + 8);
    SelectObject(dc, old); DeleteObject(pen);
}

inline void DrawUndoRedo(HDC dc, const RECT& r, bool redo) {
    const POINT c = Center(r);
    POINT pts[4];
    if (!redo) {
        pts[0] = {c.x + 7,c.y + 5}; pts[1] = {c.x + 6,c.y - 7};
        pts[2] = {c.x - 5,c.y - 7}; pts[3] = {c.x - 7,c.y + 1};
    } else {
        pts[0] = {c.x - 7,c.y + 5}; pts[1] = {c.x - 6,c.y - 7};
        pts[2] = {c.x + 5,c.y - 7}; pts[3] = {c.x + 7,c.y + 1};
    }
    HPEN pen = MakePen(dc, 2); HGDIOBJ old = SelectObject(dc, pen);
    PolyBezier(dc, pts, 4); SelectObject(dc, old); DeleteObject(pen);
    ArrowHead(dc, pts[3], pts[2], 5, 2);
}

inline void DrawCopy(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    HPEN pen = MakePen(dc, 1); HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, c.x - 7,c.y - 7,c.x + 4,c.y + 4);
    Rectangle(dc, c.x - 3,c.y - 3,c.x + 8,c.y + 8);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

inline void DrawSave(HDC dc, const RECT& r, bool saveAs) {
    const POINT c = Center(r);
    HPEN pen = MakePen(dc, 1); HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, c.x - 7,c.y - 8,c.x + 7,c.y + 8);
    Rectangle(dc, c.x - 4,c.y - 7,c.x + 4,c.y - 2);
    Rectangle(dc, c.x - 4,c.y + 2,c.x + 4,c.y + 7);
    if (saveAs) { MoveToEx(dc,c.x + 2,c.y + 7,nullptr); LineTo(dc,c.x + 9,c.y); }
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

inline void DrawCancel(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    HPEN pen = MakePen(dc, 2); HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc,c.x - 6,c.y - 6,nullptr); LineTo(dc,c.x + 6,c.y + 6);
    MoveToEx(dc,c.x + 6,c.y - 6,nullptr); LineTo(dc,c.x - 6,c.y + 6);
    SelectObject(dc, old); DeleteObject(pen);
}

inline void DrawSpecificShape(HDC dc, const RECT& r, int kind) {
    const POINT c = Center(r);
    const int l=c.x-8,t=c.y-7,rr=c.x+8,b=c.y+7;
    HPEN pen = MakePen(dc, 1); HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (kind) {
    case 0: Rectangle(dc,l,t,rr,b); break;
    case 1: RoundRect(dc,l,t,rr,b,6,6); break;
    case 2: Ellipse(dc,c.x-7,c.y-7,c.x+7,c.y+7); break;
    case 3: Ellipse(dc,l,c.y-5,rr,c.y+5); break;
    case 4: MoveToEx(dc,l,b,nullptr); LineTo(dc,rr,t); break;
    case 5: { POINT p[]={{c.x,t},{rr,b},{l,b}}; Polygon(dc,p,3); break; }
    case 6: { POINT p[]={{c.x,t},{rr,c.y},{c.x,b},{l,c.y}}; Polygon(dc,p,4); break; }
    default: { POINT p[]={{l+4,t},{rr-4,t},{rr,c.y},{rr-4,b},{l+4,b},{l,c.y}}; Polygon(dc,p,6); break; }
    }
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

inline void DrawFillMode(HDC dc, const RECT& r, int mode) {
    const POINT c = Center(r);
    RECT box{c.x-6,c.y-6,c.x+6,c.y+6};
    HPEN pen = MakePen(dc, 1); HGDIOBJ oldPen = SelectObject(dc, pen);
    HBRUSH fill = CreateSolidBrush(GetTextColor(dc));
    if (mode == 0) {
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH)); Rectangle(dc,box.left,box.top,box.right,box.bottom); SelectObject(dc,oldBrush);
    } else if (mode == 1) {
        FillRect(dc,&box,fill);
    } else {
        RECT half{box.left,(box.top+box.bottom)/2,box.right,box.bottom}; FillRect(dc,&half,fill);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH)); Rectangle(dc,box.left,box.top,box.right,box.bottom); SelectObject(dc,oldBrush);
    }
    DeleteObject(fill); SelectObject(dc,oldPen); DeleteObject(pen);
}

inline void DrawStroke(HDC dc, const RECT& r, int width) {
    const POINT c = Center(r);
    HPEN pen = MakePen(dc, width); HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc,r.left+5,c.y,nullptr); LineTo(dc,r.right-5,c.y);
    SelectObject(dc,old); DeleteObject(pen);
}

inline void DrawMore(HDC dc, const RECT& r) {
    const POINT c = Center(r);
    HBRUSH brush = CreateSolidBrush(GetTextColor(dc));
    for (int i=-1;i<=1;++i) { RECT dot{c.x+i*6-1,c.y-1,c.x+i*6+2,c.y+2}; FillRect(dc,&dot,brush); }
    DeleteObject(brush);
}

inline int DrawTextOrIcon(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format) {
    if (!dc || !text || !rect || !(format & DT_CENTER) || !(format & DT_VCENTER))
        return ::DrawTextW(dc,text,count,rect,format);

    if (Exact(text,count,L"选择")) DrawCursor(dc,*rect);
    else if (Exact(text,count,L"形状")) DrawShapeCategory(dc,*rect);
    else if (Exact(text,count,L"箭头")) DrawArrow(dc,*rect,0);
    else if (Exact(text,count,L"画笔")) DrawPen(dc,*rect);
    else if (Exact(text,count,L"马赛克")) DrawMosaic(dc,*rect);
    else if (Exact(text,count,L"文字")) DrawTextGlyph(dc,*rect);
    else if (Exact(text,count,L"图钉")) DrawPin(dc,*rect);
    else if (Exact(text,count,L"撤销")) DrawUndoRedo(dc,*rect,false);
    else if (Exact(text,count,L"重做")) DrawUndoRedo(dc,*rect,true);
    else if (Exact(text,count,L"复制")) DrawCopy(dc,*rect);
    else if (Exact(text,count,L"保存")) DrawSave(dc,*rect,false);
    else if (Exact(text,count,L"另存为")) DrawSave(dc,*rect,true);
    else if (Exact(text,count,L"取消")) DrawCancel(dc,*rect);
    else if (Exact(text,count,L"矩形")) DrawSpecificShape(dc,*rect,0);
    else if (Exact(text,count,L"圆角")) DrawSpecificShape(dc,*rect,1);
    else if (Exact(text,count,L"圆形")) DrawSpecificShape(dc,*rect,2);
    else if (Exact(text,count,L"椭圆")) DrawSpecificShape(dc,*rect,3);
    else if (Exact(text,count,L"直线")) DrawSpecificShape(dc,*rect,4);
    else if (Exact(text,count,L"三角")) DrawSpecificShape(dc,*rect,5);
    else if (Exact(text,count,L"菱形")) DrawSpecificShape(dc,*rect,6);
    else if (Exact(text,count,L"六边")) DrawSpecificShape(dc,*rect,7);
    else if (Exact(text,count,L"描边")) DrawFillMode(dc,*rect,0);
    else if (Exact(text,count,L"填充")) DrawFillMode(dc,*rect,1);
    else if (Exact(text,count,L"两者")) DrawFillMode(dc,*rect,2);
    else if (Exact(text,count,L"直箭头")) DrawArrow(dc,*rect,0);
    else if (Exact(text,count,L"细箭头")) DrawArrow(dc,*rect,1);
    else if (Exact(text,count,L"粗箭头")) DrawArrow(dc,*rect,2);
    else if (Exact(text,count,L"双向")) DrawArrow(dc,*rect,3);
    else if (Exact(text,count,L"弯曲")) DrawArrow(dc,*rect,4);
    else if (Exact(text,count,L"折线")) DrawArrow(dc,*rect,5);
    else if (Exact(text,count,L"阶梯")) DrawArrow(dc,*rect,6);
    else if (Exact(text,count,L"1") || Exact(text,count,L"细")) DrawStroke(dc,*rect,1);
    else if (Exact(text,count,L"2")) DrawStroke(dc,*rect,2);
    else if (Exact(text,count,L"普通")) DrawStroke(dc,*rect,3);
    else if (Exact(text,count,L"4")) DrawStroke(dc,*rect,4);
    else if (Exact(text,count,L"粗")) DrawStroke(dc,*rect,6);
    else if (Exact(text,count,L"6")) DrawStroke(dc,*rect,6);
    else if (Exact(text,count,L"很粗")) DrawStroke(dc,*rect,9);
    else if (Exact(text,count,L"更多")) DrawMore(dc,*rect);
    else return ::DrawTextW(dc,text,count,rect,format);
    return rect->bottom - rect->top;
}

} // namespace snaplite::toolbaricons_gdi

#define DrawTextW(...) snaplite::toolbaricons_gdi::DrawTextOrIcon(__VA_ARGS__)
