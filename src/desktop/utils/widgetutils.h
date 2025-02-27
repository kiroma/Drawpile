// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WIDGETUTILS_H
#define WIDGETUTILS_H
#include <QHBoxLayout>
#include <QIcon>
#include <QObject>
#include <QScroller>
#include <QSizePolicy>
#include <optional>

class QAbstractScrollArea;
class QBoxLayout;
class QButtonGroup;
class QCheckBox;
class QCursor;
class QFormLayout;
class QFrame;
class QHeaderView;
class QLabel;
class QWidget;

namespace utils {

class ScopedOverrideCursor {
public:
	ScopedOverrideCursor(); // Uses Qt::WaitCursor.
	ScopedOverrideCursor(const QCursor &cursor);
	~ScopedOverrideCursor();
};

class ScopedUpdateDisabler {
public:
	ScopedUpdateDisabler(QWidget *widget);
	~ScopedUpdateDisabler();

private:
	QWidget *m_widget;
	bool m_wasEnabled;
};


/// @brief Like `QHBoxLayout`, but allows the control type to be overridden to
/// provide more sensible spacing, and on macOS, fixes broken layouts caused by
/// fake extra layout margins.
class EncapsulatedLayout final : public QHBoxLayout {
	Q_OBJECT
public:
	EncapsulatedLayout();

	QSizePolicy::ControlTypes controlTypes() const override;
	void setControlTypes(QSizePolicy::ControlTypes controlTypes);

private:
	std::optional<QSizePolicy::ControlTypes> m_controlTypes;

#ifdef Q_OS_MACOS
public:
	int heightForWidth(int width) const override;
	void invalidate() override;
	QSize maximumSize() const override;
	int minimumHeightForWidth(int width) const override;
	QSize minimumSize() const override;
	void setGeometry(const QRect &rect) override;
	QSize sizeHint() const override;

private:
	int adjustHeightForWidth(int width) const;
	QSize adjustSizeHint(QSize size) const;
	void recoverEffectiveMargins() const;

	mutable bool m_dirty = true;
	mutable int m_leftMargin, m_topMargin, m_bottomMargin, m_rightMargin = 0;
#endif
};


// SPDX-SnippetBegin
// SPDX-License-Identifier: GPL-3.0-or-later
// SDPX—SnippetName: Kinetic scroll event filter from Krita
class KisKineticScrollerEventFilter : public QObject {
	Q_OBJECT
public:
	KisKineticScrollerEventFilter(
		QScroller::ScrollerGestureType gestureType,
		QAbstractScrollArea *parent);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

	QAbstractScrollArea *m_scrollArea;
	QScroller::ScrollerGestureType m_gestureType;
};
// SPDX-SnippetEnd

void showWindow(QWidget *widget, bool maximized = false);

void setWidgetRetainSizeWhenHidden(QWidget *widget, bool retainSize);

bool setGeometryIfOnScreen(QWidget *widget, const QRect &geometry);

// Sets header to sort by no column (as opposed to the first one) and enables
// clearing the sort indicator if the Qt version is >= 6.1.0.
void initSortingHeader(
	QHeaderView *header, int sortColumn = -1,
	Qt::SortOrder order = Qt::AscendingOrder);

void initKineticScrolling(QAbstractScrollArea *scrollArea);
bool isKineticScrollingBarsHidden();

QFormLayout *addFormSection(QBoxLayout *layout);

void addFormSpacer(
	QLayout *layout, QSizePolicy::Policy vPolicy = QSizePolicy::Fixed);

QFrame *makeSeparator();

void addFormSeparator(QBoxLayout *layout);

EncapsulatedLayout *encapsulate(const QString &label, QWidget *child);

EncapsulatedLayout *indent(QWidget *child);

QWidget *formNote(
	const QString &text, QSizePolicy::ControlType type = QSizePolicy::Label,
	const QIcon &icon = QIcon());

void setSpacingControlType(
	EncapsulatedLayout *widget, QSizePolicy::ControlTypes type);

void setSpacingControlType(QWidget *widget, QSizePolicy::ControlType type);

QButtonGroup *addRadioGroup(
	QFormLayout *form, const QString &label, bool horizontal,
	std::initializer_list<std::pair<const QString &, int>> items);

QCheckBox *addCheckable(
	const QString &accessibleName, EncapsulatedLayout *layout, QWidget *child);

QLabel *makeIconLabel(const QIcon &icon, QWidget *parent = nullptr);

}

#endif
