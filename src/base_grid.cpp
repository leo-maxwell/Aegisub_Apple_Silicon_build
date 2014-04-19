// Copyright (c) 2006, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "base_grid.h"

#include "include/aegisub/context.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/menu.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_box.h"
#include "compat.h"
#include "frame_main.h"
#include "grid_column.h"
#include "options.h"
#include "utils.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "video_context.h"

#include <libaegisub/util.h>

#include <algorithm>
#include <boost/range/algorithm.hpp>
#include <cmath>
#include <iterator>
#include <numeric>

#include <wx/dcbuffer.h>
#include <wx/kbdstate.h>
#include <wx/menu.h>
#include <wx/scrolbar.h>
#include <wx/sizer.h>

enum {
	GRID_SCROLLBAR = 1730,
	MENU_SHOW_COL = 1250 // Needs 15 IDs after this
};

BaseGrid::BaseGrid(wxWindow* parent, agi::Context *context)
: wxWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxSUNKEN_BORDER)
, scrollBar(new wxScrollBar(this, GRID_SCROLLBAR, wxDefaultPosition, wxDefaultSize, wxSB_VERTICAL))
, context(context)
, columns(GetGridColumns())
, seek_listener(context->videoController->AddSeekListener([&] { Refresh(false); }))
{
	scrollBar->SetScrollbar(0,10,100,10);

	auto scrollbarpositioner = new wxBoxSizer(wxHORIZONTAL);
	scrollbarpositioner->AddStretchSpacer();
	scrollbarpositioner->Add(scrollBar, 0, wxEXPAND, 0);

	SetSizerAndFit(scrollbarpositioner);

	SetBackgroundStyle(wxBG_STYLE_PAINT);

	UpdateStyle();
	OnHighlightVisibleChange(*OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame"));

	connections.push_back(context->ass->AddCommitListener(&BaseGrid::OnSubtitlesCommit, this));
	connections.push_back(context->subsController->AddFileOpenListener(&BaseGrid::OnSubtitlesOpen, this));
	connections.push_back(context->subsController->AddFileSaveListener(&BaseGrid::OnSubtitlesSave, this));

	connections.push_back(context->selectionController->AddActiveLineListener(&BaseGrid::OnActiveLineChanged, this));
	connections.push_back(context->selectionController->AddSelectionListener([&]{ Refresh(false); }));

	connections.push_back(OPT_SUB("Subtitle/Grid/Font Face", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Subtitle/Grid/Font Size", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Active Border", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Background/Background", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Background/Comment", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Background/Inframe", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Background/Selected Comment", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Background/Selection", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Collision", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Header", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Left Column", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Lines", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Selection", &BaseGrid::UpdateStyle, this));
	connections.push_back(OPT_SUB("Colour/Subtitle Grid/Standard", &BaseGrid::UpdateStyle, this));

	connections.push_back(OPT_SUB("Subtitle/Grid/Highlight Subtitles in Frame", &BaseGrid::OnHighlightVisibleChange, this));
	connections.push_back(OPT_SUB("Subtitle/Grid/Hide Overrides", [&](agi::OptionValue const&) { Refresh(false); }));

	Bind(wxEVT_CONTEXT_MENU, &BaseGrid::OnContextMenu, this);
}

BaseGrid::~BaseGrid() { }

BEGIN_EVENT_TABLE(BaseGrid,wxWindow)
	EVT_PAINT(BaseGrid::OnPaint)
	EVT_SIZE(BaseGrid::OnSize)
	EVT_COMMAND_SCROLL(GRID_SCROLLBAR,BaseGrid::OnScroll)
	EVT_MOUSE_EVENTS(BaseGrid::OnMouseEvent)
	EVT_KEY_DOWN(BaseGrid::OnKeyDown)
	EVT_CHAR_HOOK(BaseGrid::OnCharHook)
	EVT_MENU_RANGE(MENU_SHOW_COL,MENU_SHOW_COL+15,BaseGrid::OnShowColMenu)
END_EVENT_TABLE()

void BaseGrid::OnSubtitlesCommit(int type) {
	if (type == AssFile::COMMIT_NEW || type & AssFile::COMMIT_ORDER || type & AssFile::COMMIT_DIAG_ADDREM)
		UpdateMaps();

	if (type & AssFile::COMMIT_DIAG_META) {
		SetColumnWidths();
		Refresh(false);
		return;
	}
	if (type & AssFile::COMMIT_DIAG_TIME)
		Refresh(false);
	else if (type & AssFile::COMMIT_DIAG_TEXT) {
		for (auto const& rect : text_refresh_rects)
			RefreshRect(rect, false);
	}
}

void BaseGrid::OnSubtitlesOpen() {
	ScrollTo(context->ass->GetUIStateAsInt("Scroll Position"));
}

void BaseGrid::OnSubtitlesSave() {
	context->ass->SaveUIState("Scroll Position", std::to_string(yPos));
}

void BaseGrid::OnShowColMenu(wxCommandEvent &event) {
	int item = event.GetId() - MENU_SHOW_COL;
	column_shown[item] = !column_shown[item];

	OPT_SET("Subtitle/Grid/Column")->SetListBool(std::vector<bool>(std::begin(column_shown), std::end(column_shown)));

	SetColumnWidths();
	Refresh(false);
}

void BaseGrid::OnHighlightVisibleChange(agi::OptionValue const& opt) {
	if (opt.GetBool())
		seek_listener.Unblock();
	else
		seek_listener.Block();
}

void BaseGrid::UpdateStyle() {
	wxString fontname = FontFace("Subtitle/Grid");
	if (fontname.empty()) fontname = "Tahoma";
	font.SetFaceName(fontname);
	font.SetPointSize(OPT_GET("Subtitle/Grid/Font Size")->GetInt());
	font.SetWeight(wxFONTWEIGHT_NORMAL);

	wxClientDC dc(this);
	dc.SetFont(font);

	// Set line height
	lineHeight = dc.GetTextExtent("#TWFfgGhH").GetHeight() + 4;

	// Set row brushes
	row_colors.Default.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Background")->GetColor()));
	row_colors.Header.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Header")->GetColor()));
	row_colors.Selection.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selection")->GetColor()));
	row_colors.Comment.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Comment")->GetColor()));
	row_colors.Visible.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Inframe")->GetColor()));
	row_colors.SelectedComment.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selected Comment")->GetColor()));
	row_colors.LeftCol.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Left Column")->GetColor()));

	// Set column widths
	std::vector<bool> column_array(OPT_GET("Subtitle/Grid/Column")->GetListBool());
	column_shown.assign(column_array.begin(), column_array.end());
	column_shown.resize(columns.size(), true);

	column_header_widths.resize(columns.size());
	for (size_t i : agi::util::range(columns.size()))
		column_header_widths[i] = dc.GetTextExtent(columns[i]->Header()).GetWidth();

	SetColumnWidths();

	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::UpdateMaps() {
	index_line_map.clear();

	for (auto& curdiag : context->ass->Events)
		index_line_map.push_back(&curdiag);

	SetColumnWidths();
	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::OnActiveLineChanged(AssDialogue *new_active) {
	if (new_active) {
		int row = new_active->Row;
		MakeRowVisible(row);
		extendRow = row;
		Refresh(false);
	}
}

void BaseGrid::MakeRowVisible(int row) {
	int h = GetClientSize().GetHeight();

	if (row < yPos + 1)
		ScrollTo(row - 1);
	else if (row > yPos + h/lineHeight - 3)
		ScrollTo(row - h/lineHeight + 3);
}

void BaseGrid::SelectRow(int row, bool addToSelected, bool select) {
	if (row < 0 || (size_t)row >= index_line_map.size()) return;

	AssDialogue *line = index_line_map[row];

	if (!addToSelected) {
		context->selectionController->SetSelectedSet(Selection{line});
		return;
	}

	bool selected = !!context->selectionController->GetSelectedSet().count(line);
	if (select != selected) {
		auto selection = context->selectionController->GetSelectedSet();
		if (select)
			selection.insert(line);
		else
			selection.erase(line);
		context->selectionController->SetSelectedSet(std::move(selection));
	}
}

void BaseGrid::OnPaint(wxPaintEvent &) {
	// Find which columns need to be repainted
	std::vector<GridColumn *> paint_columns;
	for (wxRegionIterator region(GetUpdateRegion()); region; ++region) {
		wxRect updrect = region.GetRect();
		int x = 0;
		for (size_t i : agi::util::range(columns.size())) {
			if (updrect.x < x + column_widths[i] && updrect.x + updrect.width > x && column_widths[i])
				paint_columns.push_back(columns[i].get());
			else
				paint_columns.push_back(nullptr);
			x += column_widths[i];
		}
	}

	if (paint_columns.empty()) return;

	int w = 0;
	int h = 0;
	GetClientSize(&w,&h);
	w -= scrollBar->GetSize().GetWidth();

	wxAutoBufferedPaintDC dc(this);
	dc.SetFont(font);

	dc.SetBackground(row_colors.Default);
	dc.Clear();

	// Draw labels
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.SetBrush(row_colors.LeftCol);
	dc.DrawRectangle(0, lineHeight, column_widths[0], h-lineHeight);

	// Row colors
	wxColour text_standard(to_wx(OPT_GET("Colour/Subtitle Grid/Standard")->GetColor()));
	wxColour text_selection(to_wx(OPT_GET("Colour/Subtitle Grid/Selection")->GetColor()));
	wxColour text_collision(to_wx(OPT_GET("Colour/Subtitle Grid/Collision")->GetColor()));

	// First grid row
	wxPen grid_pen(to_wx(OPT_GET("Colour/Subtitle Grid/Lines")->GetColor()));
	dc.SetPen(grid_pen);
	dc.DrawLine(0, 0, w, 0);
	dc.SetPen(*wxTRANSPARENT_PEN);

	auto paint_text = [&](wxString const& str, int x, int y, int col) {
		wxSize ext = dc.GetTextExtent(str);

		int top = y + (lineHeight - ext.GetHeight()) / 2;
		int left = x + 4;
		if (columns[col]->Centered())
			left += (column_widths[col] - 6 - ext.GetWidth()) / 2;

		dc.DrawText(str, left, top);
	};

	// Paint header
	{
		dc.SetTextForeground(text_standard);
		dc.SetBrush(row_colors.Header);
		dc.DrawRectangle(0, 0, w, lineHeight);

		int x = 0;
		for (size_t i : agi::util::range(columns.size())) {
			if (paint_columns[i])
				paint_text(columns[i]->Header(), x, 0, i);
			x += column_widths[i];
		}

		dc.SetPen(grid_pen);
		dc.DrawLine(0, lineHeight, w, lineHeight);
	}

	// Paint the rows
	int drawPerScreen = h/lineHeight + 1;
	int nDraw = mid(0, drawPerScreen, GetRows() - yPos);

	auto active_line = context->selectionController->GetActiveLine();
	auto const& selection = context->selectionController->GetSelectedSet();

	for (int i : agi::util::range(nDraw)) {
		wxBrush color = row_colors.Default;
		AssDialogue *curDiag = index_line_map[i + yPos];

		bool inSel = !!selection.count(curDiag);
		if (inSel && curDiag->Comment)
			color = row_colors.SelectedComment;
		else if (inSel)
			color = row_colors.Selection;
		else if (curDiag->Comment)
			color = row_colors.Comment;
		else if (OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame")->GetBool() && IsDisplayed(curDiag))
			color = row_colors.Visible;

		if (active_line != curDiag && curDiag->CollidesWith(active_line))
			dc.SetTextForeground(text_collision);
		else if (inSel)
			dc.SetTextForeground(text_selection);
		else
			dc.SetTextForeground(text_standard);

		// Draw row background color
		if (color != row_colors.Default) {
			dc.SetBrush(color);
			dc.DrawRectangle(column_widths[0], (i + 1) * lineHeight + 1, w, lineHeight);
		}

		// Draw text
		int x = 0;
		int y = (i + 1) * lineHeight;
		for (size_t j : agi::util::range(columns.size())) {
			if (column_widths[j] == 0) continue;

			if (paint_columns[j])
				paint_text(columns[j]->Value(curDiag, context), x, y, j);
			x += column_widths[j];
		}

		// Draw grid
		if (curDiag == active_line) {
			dc.SetPen(wxPen(to_wx(OPT_GET("Colour/Subtitle Grid/Active Border")->GetColor())));
			dc.SetBrush(*wxTRANSPARENT_BRUSH);
			dc.DrawRectangle(0, y, w, lineHeight + 1);
		}
		else {
			dc.SetPen(grid_pen);
			dc.DrawLine(0, y + lineHeight, w , y + lineHeight);
		}
		dc.SetPen(*wxTRANSPARENT_PEN);
	}

	// Draw grid columns
	{
		int maxH = (nDraw + 1) * lineHeight;
		int x = 0;
		dc.SetPen(grid_pen);
		for (int width : column_widths) {
			x += width;
			if (x < w)
				dc.DrawLine(x, 0, x, maxH);
		}
		dc.DrawLine(0, 0, 0, maxH);
		dc.DrawLine(w - 1, 0, w - 1, maxH);
	}
}

void BaseGrid::OnSize(wxSizeEvent &) {
	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::OnScroll(wxScrollEvent &event) {
	int newPos = event.GetPosition();
	if (yPos != newPos) {
		yPos = newPos;
		Refresh(false);
	}
}

void BaseGrid::OnMouseEvent(wxMouseEvent &event) {
	int h = GetClientSize().GetHeight();
	bool shift = event.ShiftDown();
	bool alt = event.AltDown();
	bool ctrl = event.CmdDown();

	// Row that mouse is over
	bool click = event.LeftDown();
	bool dclick = event.LeftDClick();
	int row = event.GetY() / lineHeight + yPos - 1;
	if (holding && !click)
		row = mid(0, row, GetRows()-1);
	AssDialogue *dlg = GetDialogue(row);
	if (!dlg) row = 0;

	if (event.ButtonDown() && OPT_GET("Subtitle/Grid/Focus Allow")->GetBool())
		SetFocus();

	if (holding) {
		if (!event.LeftIsDown()) {
			if (dlg)
				MakeRowVisible(row);
			holding = false;
			ReleaseMouse();
		}
		else {
			// Only scroll if the mouse has moved to a different row to avoid
			// scrolling on sloppy clicks
			if (row != extendRow) {
				if (row <= yPos)
					ScrollTo(yPos - 3);
				// When dragging down we give a 3 row margin to make it easier
				// to see what's going on, but we don't want to scroll down if
				// the user clicks on the bottom row and drags up
				else if (row > yPos + h / lineHeight - (row > extendRow ? 3 : 1))
					ScrollTo(yPos + 3);
			}
		}
	}
	else if (click && dlg) {
		holding = true;
		CaptureMouse();
	}

	if ((click || holding || dclick) && dlg) {
		int old_extend = extendRow;

		// SetActiveLine will scroll the grid if the row is only half-visible,
		// but we don't want to scroll until the mouse moves or the button is
		// released, to avoid selecting multiple lines on a click
		int old_y_pos = yPos;
		context->selectionController->SetActiveLine(dlg);
		ScrollTo(old_y_pos);
		extendRow = row;

		auto const& selection = context->selectionController->GetSelectedSet();

		// Toggle selected
		if (click && ctrl && !shift && !alt) {
			bool isSel = !!selection.count(dlg);
			if (isSel && selection.size() == 1) return;
			SelectRow(row, true, !isSel);
			return;
		}

		// Normal click
		if ((click || dclick) && !shift && !ctrl && !alt) {
			if (dclick) {
				context->audioBox->ScrollToActiveLine();
				context->videoController->JumpToTime(dlg->Start);
			}
			SelectRow(row, false);
			return;
		}

		// Change active line only
		if (click && !shift && !ctrl && alt)
			return;

		// Block select
		if ((click && shift && !alt) || holding) {
			extendRow = old_extend;
			int i1 = row;
			int i2 = extendRow;

			if (i1 > i2)
				std::swap(i1, i2);

			// Toggle each
			Selection newsel;
			if (ctrl) newsel = selection;
			for (int i = i1; i <= i2; i++)
				newsel.insert(GetDialogue(i));
			context->selectionController->SetSelectedSet(std::move(newsel));
			return;
		}

		return;
	}

	// Mouse wheel
	if (event.GetWheelRotation() != 0) {
		if (ForwardMouseWheelEvent(this, event)) {
			int step = shift ? h / lineHeight - 2 : 3;
			ScrollTo(yPos - step * event.GetWheelRotation() / event.GetWheelDelta());
		}
		return;
	}

	event.Skip();
}

void BaseGrid::OnContextMenu(wxContextMenuEvent &evt) {
	wxPoint pos = evt.GetPosition();
	if (pos == wxDefaultPosition || ScreenToClient(pos).y > lineHeight) {
		if (!context_menu) context_menu = menu::GetMenu("grid_context", context);
		menu::OpenPopupMenu(context_menu.get(), this);
	}
	else {
		wxMenu menu;
		for (size_t i : agi::util::range(columns.size())) {
			if (columns[i]->CanHide())
				menu.Append(MENU_SHOW_COL + i, columns[i]->Description(), "", wxITEM_CHECK)->Check(!!column_shown[i]);
		}
		PopupMenu(&menu);
	}
}

void BaseGrid::ScrollTo(int y) {
	int nextY = mid(0, y, GetRows() - 1);
	if (yPos != nextY) {
		yPos = nextY;
		scrollBar->SetThumbPosition(yPos);
		Refresh(false);
	}
}

void BaseGrid::AdjustScrollbar() {
	wxSize clientSize = GetClientSize();
	wxSize scrollbarSize = scrollBar->GetSize();

	scrollBar->Freeze();
	scrollBar->SetSize(clientSize.GetWidth() - scrollbarSize.GetWidth(), 0, scrollbarSize.GetWidth(), clientSize.GetHeight());

	if (GetRows() <= 1) {
		scrollBar->Enable(false);
		scrollBar->Thaw();
		return;
	}

	if (!scrollBar->IsEnabled())
		scrollBar->Enable(true);

	int drawPerScreen = clientSize.GetHeight() / lineHeight;
	int rows = GetRows();

	yPos = mid(0, yPos, rows - 1);

	scrollBar->SetScrollbar(yPos, drawPerScreen, rows + drawPerScreen - 1, drawPerScreen - 2, true);
	scrollBar->Thaw();
}

void BaseGrid::SetColumnWidths() {
	int w, h;
	GetClientSize(&w, &h);

	// DC for text extents test
	wxClientDC dc(this);
	dc.SetFont(font);

	WidthHelper helper{dc, std::unordered_map<boost::flyweight<std::string>, int>{}};

	column_widths.clear();
	for (auto i : agi::util::range(columns.size())) {
		if (!column_shown[i])
			column_widths.push_back(0);
		else {
			int width = columns[i]->Width(context, helper);
			if (width) // 10 is an arbitrary amount of padding
				width = 10 + std::max(width, column_header_widths[i]);
			column_widths.push_back(width);
		}
	}

	text_refresh_rects.clear();
	int x = 0;
	for (auto i : agi::util::range(columns.size())) {
		if (columns[i]->RefreshOnTextChange() && column_widths[i])
			text_refresh_rects.emplace_back(x, 0, column_widths[i], h);
	}
}

AssDialogue *BaseGrid::GetDialogue(int n) const {
	if (static_cast<size_t>(n) >= index_line_map.size()) return nullptr;
	return index_line_map[n];
}

bool BaseGrid::IsDisplayed(const AssDialogue *line) const {
	if (!context->videoController->IsLoaded()) return false;
	int frame = context->videoController->GetFrameN();
	return context->videoController->FrameAtTime(line->Start,agi::vfr::START) <= frame
		&& context->videoController->FrameAtTime(line->End,agi::vfr::END) >= frame;
}

void BaseGrid::OnCharHook(wxKeyEvent &event) {
	if (hotkey::check("Subtitle Grid", context, event))
		return;

	int key = event.GetKeyCode();

	if (key == WXK_UP || key == WXK_DOWN ||
		key == WXK_PAGEUP || key == WXK_PAGEDOWN ||
		key == WXK_HOME || key == WXK_END)
	{
		event.Skip();
		return;
	}

	hotkey::check("Audio", context, event);
}

void BaseGrid::OnKeyDown(wxKeyEvent &event) {
	int w,h;
	GetClientSize(&w, &h);

	int key = event.GetKeyCode();
	bool ctrl = event.CmdDown();
	bool alt = event.AltDown();
	bool shift = event.ShiftDown();

	int dir = 0;
	int step = 1;
	if (key == WXK_UP) dir = -1;
	else if (key == WXK_DOWN) dir = 1;
	else if (key == WXK_PAGEUP) {
		dir = -1;
		step = h / lineHeight - 2;
	}
	else if (key == WXK_PAGEDOWN) {
		dir = 1;
		step = h / lineHeight - 2;
	}
	else if (key == WXK_HOME) {
		dir = -1;
		step = GetRows();
	}
	else if (key == WXK_END) {
		dir = 1;
		step = GetRows();
	}

	if (!dir) {
		event.Skip();
		return;
	}

	auto active_line = context->selectionController->GetActiveLine();
	int old_extend = extendRow;
	int next = mid(0, (active_line ? active_line->Row : 0) + dir * step, GetRows() - 1);
	context->selectionController->SetActiveLine(GetDialogue(next));

	// Move selection
	if (!ctrl && !shift && !alt) {
		SelectRow(next);
		return;
	}

	// Move active only
	if (alt && !shift && !ctrl)
		return;

	// Shift-selection
	if (shift && !ctrl && !alt) {
		extendRow = old_extend;
		// Set range
		int begin = next;
		int end = extendRow;
		if (end < begin)
			std::swap(begin, end);

		// Select range
		Selection newsel;
		for (int i = begin; i <= end; i++)
			newsel.insert(GetDialogue(i));

		context->selectionController->SetSelectedSet(std::move(newsel));

		MakeRowVisible(next);
		return;
	}
}

void BaseGrid::SetByFrame(bool state) {
	if (byFrame == state) return;
	byFrame = state;
	for (auto& column : columns)
		column->SetByFrame(byFrame);
	SetColumnWidths();
	Refresh(false);
}
