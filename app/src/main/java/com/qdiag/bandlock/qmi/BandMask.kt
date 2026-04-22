package com.qdiag.bandlock.qmi

/**
 * Bitmask helpers for LTE and NR5G band preferences, encoded the way libqmi /
 * QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE expects them: bit (N-1) is band N,
 * split across two 64-bit words to cover bands 1..128.
 */
data class BandMask(val low: Long, val high: Long) {
    fun contains(band: Int): Boolean = when {
        band < 1 || band > 128 -> false
        band <= 64 -> (low ushr (band - 1)) and 1L == 1L
        else       -> (high ushr (band - 65)) and 1L == 1L
    }

    fun with(band: Int, enabled: Boolean): BandMask {
        if (band < 1 || band > 128) return this
        return if (band <= 64) {
            val bit = 1L shl (band - 1)
            copy(low = if (enabled) low or bit else low and bit.inv())
        } else {
            val bit = 1L shl (band - 65)
            copy(high = if (enabled) high or bit else high and bit.inv())
        }
    }

    fun enabledBands(): List<Int> = buildList {
        for (b in 1..64)  if ((low ushr (b - 1)) and 1L == 1L) add(b)
        for (b in 65..128) if ((high ushr (b - 65)) and 1L == 1L) add(b)
    }

    companion object {
        val NONE = BandMask(0L, 0L)
        val ALL  = BandMask(-1L /* = 0xFFFF...FF */, -1L)
        fun of(vararg bands: Int): BandMask =
            bands.fold(NONE) { acc, b -> acc.with(b, true) }
    }
}

/**
 * Bands actually supported by the Snapdragon X70 modem on Poco F6
 * (Global model). Entries that the modem rejected with QMI_ERR_INTERNAL
 * during empirical testing (B2/B4/B6, various mid-/high-/mmW NR) are
 * omitted so the user can't pick something the modem will refuse.
 */
object BandCatalog {
    data class Entry(val number: Int, val freqLabel: String)

    val LTE: List<Entry> = listOf(
        Entry(1,  "2100"),
        Entry(3,  "1800+"),
        Entry(5,  "850"),
        Entry(7,  "2600"),
        Entry(8,  "900"),
        Entry(20, "800 DD"),
        Entry(28, "700 APT"),
        Entry(38, "TDD 2600"),
        Entry(40, "TDD 2300"),
        Entry(41, "TDD 2500"),
    )

    val NR: List<Entry> = listOf(
        Entry(1,  "n1 2100"),
        Entry(3,  "n3 1800"),
        Entry(5,  "n5 850"),
        Entry(7,  "n7 2600"),
        Entry(8,  "n8 900"),
        Entry(20, "n20 800"),
        Entry(28, "n28 700"),
        Entry(38, "n38 TDD 2600"),
        Entry(40, "n40 TDD 2300"),
        Entry(41, "n41 TDD 2500"),
        Entry(77, "n77 TDD 3700"),
        Entry(78, "n78 TDD 3500"),
        Entry(79, "n79 TDD 4700"),
    )
}
