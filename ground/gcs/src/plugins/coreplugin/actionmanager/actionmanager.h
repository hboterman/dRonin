/**
 ******************************************************************************
 *
 * @file       actionmanager.h
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 *             Parts by Nokia Corporation (qt-info@nokia.com) Copyright (C) 2009.
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup CorePlugin Core Plugin
 * @{
 * @brief The Core GCS plugin
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */

#ifndef ACTIONMANAGER_H
#define ACTIONMANAGER_H

#include "coreplugin/core_global.h"

#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/actionmanager/command.h>

#include <QtCore/QObject>
#include <QtCore/QList>

QT_BEGIN_NAMESPACE
class QAction;
class QShortcut;
class QString;
QT_END_NAMESPACE

namespace Core {

class CORE_EXPORT ActionManager : public QObject
{
    Q_OBJECT
public:
    ActionManager(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    virtual ~ActionManager() {}

    virtual ActionContainer *createMenu(const QString &id) = 0;
    virtual ActionContainer *createMenuBar(const QString &id) = 0;

    virtual Command *registerAction(QAction *action, const QString &id,
                                    const QList<int> &context) = 0;
    virtual Command *registerShortcut(QShortcut *shortcut, const QString &id,
                                      const QList<int> &context) = 0;

    virtual Command *command(const QString &id) const = 0;
    virtual ActionContainer *actionContainer(const QString &id) const = 0;
};

} // namespace Core

#endif // ACTIONMANAGER_H
