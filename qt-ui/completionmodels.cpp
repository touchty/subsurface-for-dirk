#include "completionmodels.h"
#include "dive.h"
#include "mainwindow.h"

#define CREATE_SINGLETON(X) \
X* X::instance() \
{ \
	static QScopedPointer<X> self(new X()); \
	return self.data(); \
}

CREATE_SINGLETON(BuddyCompletionModel);
CREATE_SINGLETON(DiveMasterCompletionModel);
CREATE_SINGLETON(LocationCompletionModel);
CREATE_SINGLETON(SuitCompletionModel);
CREATE_SINGLETON(TagCompletionModel);

#undef CREATE_SINGLETON

#define CREATE_UPDATE_METHOD(Class, diveStructMember) \
void Class::updateModel() \
{ \
	QStringList list; \
	struct dive* dive; \
	int i = 0; \
	for_each_dive(i, dive){ \
		QString buddy(dive->diveStructMember); \
		if (!list.contains(buddy)){ \
			list.append(buddy); \
		} \
	} \
	setStringList(list); \
}

void BuddyCompletionModel::updateModel()
{
	QSet<QString> set;
	struct dive* dive;
	int i = 0;
	for_each_dive(i, dive){
		QString buddy(dive->buddy);
		foreach (const QString &value, buddy.split(",", QString::SkipEmptyParts)) {
			set.insert(value.trimmed());
		}
	}
	setStringList(set.toList());
}

CREATE_UPDATE_METHOD(DiveMasterCompletionModel, divemaster);
CREATE_UPDATE_METHOD(LocationCompletionModel, location);
CREATE_UPDATE_METHOD(SuitCompletionModel, suit);

void TagCompletionModel::updateModel()
{
	if(g_tag_list == NULL)
		return;
	QStringList list;
	struct tag_entry *current_tag_entry = g_tag_list->next;
	while (current_tag_entry != NULL) {
		list.append(QString(current_tag_entry->tag->name));
		current_tag_entry = current_tag_entry->next;
	}
	setStringList(list);
}
