/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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
 *
 */

/*
 * @test
 * @summary Verify that non-power-of-2 alloc region counts are rejected at startup
 * @bug 8361099
 * @requires vm.gc.Shenandoah
 * @library /test/lib
 * @modules java.base/jdk.internal.misc
 *          java.management
 * @run driver TestAllocRegionsPowerOfTwo
 */

import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;

public class TestAllocRegionsPowerOfTwo {
    public static void main(String[] args) throws Exception {
        // Non-power-of-2 values must be rejected for ShenandoahMutatorAllocRegions
        for (int bad : new int[]{3, 5, 6, 7, 9, 10, 12, 15, 17, 24, 31}) {
            OutputAnalyzer output = ProcessTools.executeLimitedTestJava(
                    "-Xmx128m",
                    "-XX:+UnlockDiagnosticVMOptions",
                    "-XX:+UnlockExperimentalVMOptions",
                    "-XX:+UseShenandoahGC",
                    "-XX:ShenandoahMutatorAllocRegions=" + bad,
                    "-version");
            output.shouldContain("ShenandoahMutatorAllocRegions to be a power of 2");
            output.shouldNotHaveExitValue(0);
        }

        // Non-power-of-2 values must be rejected for ShenandoahCollectorAllocRegions
        for (int bad : new int[]{3, 5, 6, 7, 9, 10, 12, 15, 17, 24, 31}) {
            OutputAnalyzer output = ProcessTools.executeLimitedTestJava(
                    "-Xmx128m",
                    "-XX:+UnlockDiagnosticVMOptions",
                    "-XX:+UnlockExperimentalVMOptions",
                    "-XX:+UseShenandoahGC",
                    "-XX:ShenandoahCollectorAllocRegions=" + bad,
                    "-version");
            output.shouldContain("ShenandoahCollectorAllocRegions to be a power of 2");
            output.shouldNotHaveExitValue(0);
        }

        // Valid power-of-2 values must be accepted
        for (int good : new int[]{0, 1, 2, 4, 8, 16, 32}) {
            OutputAnalyzer output = ProcessTools.executeLimitedTestJava(
                    "-Xmx256m",
                    "-XX:+UnlockDiagnosticVMOptions",
                    "-XX:+UnlockExperimentalVMOptions",
                    "-XX:+UseShenandoahGC",
                    "-XX:ShenandoahMutatorAllocRegions=" + good,
                    "-version");
            output.shouldHaveExitValue(0);
        }
        for (int good : new int[]{0, 1, 2, 4, 8, 16, 32}) {
            OutputAnalyzer output = ProcessTools.executeLimitedTestJava(
                    "-Xmx256m",
                    "-XX:+UnlockDiagnosticVMOptions",
                    "-XX:+UnlockExperimentalVMOptions",
                    "-XX:+UseShenandoahGC",
                    "-XX:ShenandoahCollectorAllocRegions=" + good,
                    "-version");
            output.shouldHaveExitValue(0);
        }
    }
}
