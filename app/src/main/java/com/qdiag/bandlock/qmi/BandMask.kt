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

/** Curated catalog of the bands most people care about. */
object BandCatalog {
    data class Entry(val number: Int, val freqLabel: String)

    val LTE: List<Entry> = listOf(
        Entry(1,  "2100"),  Entry(2,  "1900 PCS"), Entry(3,  "1800+"), Entry(4, "AWS-1 1700/2100"),
        Entry(5,  "850"),   Entry(7,  "2600"),     Entry(8,  "900"),   Entry(12, "700 a"),
        Entry(13, "700 c"), Entry(14, "700 PS"),   Entry(17, "700 b"), Entry(18, "800 Lower"),
        Entry(19, "800 Upper"), Entry(20, "800 DD"), Entry(25, "1900+"), Entry(26, "850+"),
        Entry(28, "700 APT"),  Entry(29, "700 d"),  Entry(30, "2300 WCS"),
        Entry(38, "TDD 2600"), Entry(39, "TDD 1900"), Entry(40, "TDD 2300"),
        Entry(41, "TDD 2500"), Entry(42, "TDD 3500"), Entry(46, "TDD 5200 LAA"),
        Entry(48, "TDD 3600 CBRS"), Entry(66, "AWS-3"), Entry(71, "600"),
    )

    val NR: List<Entry> = listOf(
        Entry(1,  "n1 2100"),  Entry(2, "n2 1900"), Entry(3, "n3 1800"), Entry(5, "n5 850"),
        Entry(7,  "n7 2600"),  Entry(8, "n8 900"),  Entry(20, "n20 800"), Entry(25, "n25 1900+"),
        Entry(28, "n28 700"),  Entry(38, "n38 TDD 2600"),
        Entry(40, "n40 TDD 2300"), Entry(41, "n41 TDD 2500"),
        Entry(66, "n66 AWS-3"), Entry(71, "n71 600"),
        Entry(77, "n77 TDD 3700"), Entry(78, "n78 TDD 3500"),
        Entry(79, "n79 TDD 4700"), Entry(257, "n257 28 GHz mmW"),
        Entry(258, "n258 26 GHz mmW"), Entry(260, "n260 39 GHz mmW"),
        Entry(261, "n261 28 GHz mmW"),
    )
}
