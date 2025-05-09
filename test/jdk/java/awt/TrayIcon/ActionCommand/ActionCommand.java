/*
 * Copyright (c) 2007, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import jdk.test.lib.Platform;
import jtreg.SkippedException;

import java.awt.AWTException;
import java.awt.EventQueue;
import java.awt.Point;
import java.awt.SystemTray;
import java.awt.TrayIcon;
import java.awt.image.BufferedImage;

/*
 * @test
 * @key headful
 * @summary Check the return value of the getActionCommand method
 *          of the ActionEvent triggered when TrayIcon is double clicked
 *          (single clicked, on Mac)
 * @modules java.desktop/java.awt:open
 * @library
 *          /java/awt/patchlib
 *          /java/awt/TrayIcon
 *          /lib/client
 *          /test/lib
 * @build
 *          java.desktop/java.awt.Helper
 *          jdk.test.lib.Platform
 *          jtreg.SkippedException
 *          ExtendedRobot
 *          SystemTrayIconHelper
 * @run main ActionCommand
 */

public class ActionCommand {

    TrayIcon icon;
    ExtendedRobot robot;

    volatile boolean actionPerformed = false;
    volatile String actionCommand = null;
    final Object actionLock = new Object();

    static boolean isMacOS = false;

    public static void main(String[] args) throws Exception {
        if (Platform.isOnWayland()) {
            // The current robot implementation does not support
            // clicking in the system tray area.
            throw new SkippedException("Skipped on Wayland");
        }

        if (!SystemTray.isSupported()) {
            throw new SkippedException("SystemTray is not supported on this platform.");
        }

        if (Platform.isWindows()) {
            System.err.println("Test can fail if the icon hides to a tray icons pool " +
                    "in Windows 7, which is behavior by default.\n" +
                    "Set \"Right mouse click\" -> \"Customize notification icons\" -> " +
                    "\"Always show all icons and notifications on the taskbar\" true to " +
                    "avoid this problem. Or change behavior only for Java SE tray icon " +
                    "and rerun test.");
        } else if (Platform.isOSX()){
            isMacOS = true;
        } else if (SystemTrayIconHelper.isOel7orLater()) {
            System.out.println("OEL 7 doesn't support double click in " +
                    "systray. Skipped");
            throw new SkippedException("Skipped on OEL 7+");
        }
        new ActionCommand().doTest();
    }

    void doTest() throws Exception {
        robot = new ExtendedRobot();

        EventQueue.invokeAndWait(() -> {
            SystemTray tray = SystemTray.getSystemTray();
            icon = new TrayIcon(new BufferedImage(20, 20, BufferedImage.TYPE_INT_RGB), "Sample Icon");
            icon.addActionListener((event) -> {
                actionPerformed = true;
                actionCommand = event.getActionCommand();
                synchronized (actionLock) {
                    try {
                        actionLock.notifyAll();
                    } catch (Exception e) {
                    }
                }
            });

            if (icon.getActionCommand() != null)
                throw new RuntimeException("FAIL: getActionCommand did not return null " +
                        "when no action command set " + icon.getActionCommand());

            icon.setActionCommand("Sample Command");

            if (!"Sample Command".equals(icon.getActionCommand()))
                throw new RuntimeException("FAIL: getActionCommand did not return the correct value. " +
                        icon.getActionCommand() + " Expected: Sample Command");

            try {
                tray.add(icon);
            } catch (AWTException e) {
                throw new RuntimeException(e);
            }
        });

        robot.waitForIdle();

        Point iconPosition = SystemTrayIconHelper.getTrayIconLocation(icon);
        if (iconPosition == null)
            throw new RuntimeException("Unable to find the icon location!");

        robot.mouseMove(iconPosition.x, iconPosition.y);
        robot.waitForIdle(2000);
        actionPerformed = false;
        SystemTrayIconHelper.doubleClick(robot);

        if (!actionPerformed) {
            synchronized (actionLock) {
                try {
                    actionLock.wait(3000);
                } catch (Exception e) {
                }
            }
        }
        if (!actionPerformed) {
            throw new RuntimeException("FAIL: ActionEvent not triggered when TrayIcon is "+(isMacOS? "" : "double ")+"clicked");
        } else if (! "Sample Command".equals(actionCommand)) {
            throw new RuntimeException("FAIL: ActionEvent.getActionCommand did not return the correct " +
                    "value. Returned: " + actionCommand + " ; Expected: Sample Command");
        }

        EventQueue.invokeAndWait(() -> {
            icon.setActionCommand(null);
            if (icon.getActionCommand() != null) {
                throw new RuntimeException("FAIL: ActionCommand set to null. getActionCommand did " +
                        "not return null " + icon.getActionCommand());
            }
        });

        robot.mouseMove(100, 0);
        robot.waitForIdle();
        robot.mouseMove(iconPosition.x, iconPosition.y);
        robot.waitForIdle();

        actionPerformed = false;
        actionCommand = null;
        SystemTrayIconHelper.doubleClick(robot);

        if (!actionPerformed) {
            synchronized (actionLock) {
                try {
                    actionLock.wait(3000);
                } catch (Exception e) {
                }
            }
        }
        if (!actionPerformed) {
            throw new RuntimeException("FAIL: ActionEvent not triggered when ActionCommand set to " +
                    "null and then TrayIcon is "+(isMacOS? "" : "double ")+ "clicked");
        } else if (actionCommand != null) {
            throw new RuntimeException("FAIL: ActionEvent.getActionCommand did not return null " +
                    "when ActionCommand is set to null " + actionCommand);
        }

    }
}
