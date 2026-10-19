package me.phie.tawc.install

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Metadata parsing for [Installation.systemdBoot]: off by default,
 *  round-trips, omitted from JSON while off. */
class InstallationSystemdTest {

    private val minimal = """{ "id": "debian", "arch": "arm64-v8a" }"""

    @Test
    fun defaultsToOffAndIsOmitted() {
        val inst = Installation.fromJson(minimal)
        assertFalse(inst.systemdBoot)
        assertFalse(inst.toJson().contains("systemdBoot"))
    }

    @Test
    fun roundTrips() {
        val on = Installation.fromJson(minimal).copy(systemdBoot = true)
        assertTrue(Installation.fromJson(on.toJson()).systemdBoot)
    }
}
