package com.qdiag.bandlock.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Menu
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.NavigationDrawerItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.qdiag.bandlock.qmi.BandCatalog
import com.qdiag.bandlock.qmi.BandMask
import com.qdiag.bandlock.root.RootClient
import com.qdiag.bandlock.telephony.Rat
import com.qdiag.bandlock.telephony.RatSnapshot
import kotlinx.coroutines.launch

private enum class DrawerPanel { None, BandLock, CellLock, NvInspector, Sniffer, Log }

private val RAT_ORDER = listOf(Rat.GSM, Rat.WCDMA, Rat.LTE, Rat.NR)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(vm: MainViewModel) {
    val state by vm.ui.collectAsStateWithLifecycle()
    val drawerState = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    var activePanel by remember { mutableStateOf(DrawerPanel.None) }
    var variant by rememberSaveable { mutableStateOf(DesignVariant.Nsg) }
    val glass = variant == DesignVariant.Glass
    val snackbar = remember { SnackbarHostState() }
    LaunchedEffect(vm) {
        vm.toasts.collect { snackbar.showSnackbar(it) }
    }

    val chromeText      = if (glass) GlassColors.TextPrimary else NsgColors.TextPrimary
    val chromeLabel     = if (glass) GlassColors.TextSecondary else NsgColors.TextLabel
    val chromeAccent    = if (glass) GlassColors.Accent else NsgColors.Accent
    val chromeDivider   = if (glass) GlassColors.DividerSoft else NsgColors.Divider
    val chromeBgColor   = if (glass) Color.Transparent else NsgColors.Background
    val chromeSurface   = if (glass) Color.Transparent else NsgColors.Surface
    // In Glass mode the drawer sheet must be nearly opaque — otherwise the main
    // screen content bleeds through and all drawer labels become unreadable.
    val drawerContainer = if (glass) Color(0xFF0C1120) else NsgColors.Surface

    val ui: @Composable () -> Unit = {
        ModalNavigationDrawer(
            drawerState = drawerState,
            drawerContent = {
                ModalDrawerSheet(
                    drawerContainerColor = drawerContainer,
                ) {
                    Text(
                        "FoxikNetwork",
                        color = chromeAccent,
                        fontWeight = FontWeight.Bold,
                        fontSize = 18.sp,
                        modifier = Modifier.padding(16.dp),
                    )
                    HorizontalDivider(color = chromeDivider)
                    DrawerRow("Band Lock (LTE / NR)", chromeText) {
                        activePanel = DrawerPanel.BandLock
                        scope.launch { drawerState.close() }
                    }
                    DrawerRow("Cell Lock (LTE PCI)", chromeText) {
                        activePanel = DrawerPanel.CellLock
                        scope.launch { drawerState.close() }
                    }
                    DrawerRow("NV Inspector (EFS2)", chromeText) {
                        activePanel = DrawerPanel.NvInspector
                        scope.launch { drawerState.close() }
                    }
                    DrawerRow("DIAG Sniffer", chromeText) {
                        activePanel = DrawerPanel.Sniffer
                        scope.launch { drawerState.close() }
                    }
                    DrawerRow("Command Log", chromeText) {
                        activePanel = DrawerPanel.Log
                        scope.launch { drawerState.close() }
                    }
                    HorizontalDivider(color = chromeDivider)
                    DrawerRow(
                        if (glass) "Theme: Liquid Glass (iOS) → tap to switch to NSG"
                        else       "Theme: NSG (dark) → tap to switch to Liquid Glass",
                        chromeAccent,
                    ) {
                        variant = if (glass) DesignVariant.Nsg else DesignVariant.Glass
                        scope.launch { drawerState.close() }
                    }
                    HorizontalDivider(color = chromeDivider)
                    DrawerRow("Close panel", chromeText) {
                        activePanel = DrawerPanel.None
                        scope.launch { drawerState.close() }
                    }
                }
            },
        ) {
            Scaffold(
                containerColor = chromeBgColor,
                snackbarHost = { SnackbarHost(snackbar) },
                topBar = {
                    TopAppBar(
                        title = { Text("FoxikNetwork", color = chromeAccent, fontWeight = FontWeight.Bold) },
                        navigationIcon = {
                            IconButton(onClick = { scope.launch { drawerState.open() } }) {
                                Icon(Icons.Default.Menu, "menu", tint = chromeText)
                            }
                        },
                        actions = {
                            Text(
                                text = "root:${rootShort(state.rootStatus)}  " +
                                    "svc:${if (state.apiBound) "ok" else "-"}  " +
                                    "diag:${if (state.diagOpen) "open" else "-"}",
                                color = chromeLabel,
                                fontSize = 12.sp,
                                modifier = Modifier.padding(end = 12.dp),
                            )
                        },
                        colors = TopAppBarDefaults.topAppBarColors(
                            containerColor = chromeSurface,
                            titleContentColor = chromeAccent,
                        ),
                    )
                },
            ) { padding ->
                if (glass && activePanel == DrawerPanel.None) {
                    GlassHome(
                        state = state,
                        vm = vm,
                        paddingValues = padding,
                    )
                } else {
                    Column(
                        Modifier
                            .padding(padding)
                            .fillMaxSize(),
                    ) {
                        val ratModBase =
                            if (activePanel == DrawerPanel.None) Modifier.weight(1f)
                            else Modifier.heightIn(max = 180.dp)
                        val ratMod = if (glass) {
                            ratModBase
                                .fillMaxWidth()
                                .padding(horizontal = 12.dp, vertical = 8.dp)
                                .glassPanel(cornerDp = 22)
                                .padding(horizontal = 4.dp, vertical = 6.dp)
                        } else ratModBase
                        RatPagerSection(state, ratMod)
                        if (activePanel != DrawerPanel.None) {
                            if (!glass) HorizontalDivider(color = chromeDivider)
                            Box(
                                modifier = if (glass) {
                                    Modifier
                                        .fillMaxWidth()
                                        .weight(1f, fill = true)
                                        .padding(12.dp)
                                        .glassPanel(cornerDp = 24)
                                        .padding(14.dp)
                                } else {
                                    Modifier
                                        .fillMaxWidth()
                                        .weight(1f, fill = true)
                                        .background(NsgColors.Surface)
                                        .padding(12.dp)
                                },
                            ) {
                                when (activePanel) {
                                    DrawerPanel.BandLock    ->
                                        if (glass) GlassBandLockPanel(state, vm)
                                        else       BandLockPanel(state, vm)
                                    DrawerPanel.CellLock    -> CellLockPanel(state, vm)
                                    DrawerPanel.NvInspector -> NvInspectorPanel(state, vm)
                                    DrawerPanel.Sniffer     -> SnifferPanel(state, vm)
                                    DrawerPanel.Log         -> LogPanel(state, vm)
                                    DrawerPanel.None        -> {}
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    CompositionLocalProvider(LocalDesignVariant provides variant) {
        if (glass) GlassBackdrop { ui() } else ui()
    }
}

/* =======================================================================
 *  Liquid-Glass home screen: three stacked cards like the FoxikNetwork
 *  mockup — (1) RAT snapshot pager (2) Band preference (3) LTE Cell Table.
 *  Entire screen is a single verticalScroll so the three cards flow on
 *  phones of any height. No nested LazyColumns.
 * ======================================================================= */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GlassHome(
    state: UiState,
    vm: MainViewModel,
    paddingValues: PaddingValues,
) {
    Column(
        modifier = Modifier
            .padding(paddingValues)
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        /* ---- Card 1: RAT pager ---------------------------------------- */
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(400.dp)
                .glassPanel(cornerDp = 24)
                .padding(horizontal = 4.dp, vertical = 6.dp),
        ) {
            RatPagerSection(state, Modifier.fillMaxSize())
        }

        /* ---- Card 2: Band preference ---------------------------------- */
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .glassPanel(cornerDp = 24)
                .padding(horizontal = 16.dp, vertical = 16.dp),
        ) {
            val lteCount = state.lteMask.enabledBands().size
            val nrCount  = state.nrMask.enabledBands().size
            Text(
                "Band preference",
                color = GlassColors.TextPrimary,
                fontWeight = FontWeight.Bold,
                fontSize = 20.sp,
            )
            Text(
                "LTE: $lteCount selected   •   NR5G: $nrCount selected",
                color = GlassColors.TextSecondary,
                fontSize = 12.sp,
                modifier = Modifier.padding(top = 2.dp),
            )
            Spacer(Modifier.height(14.dp))
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp)
                    .glassPrimaryPill()
                    .clickable(enabled = !state.busy) { vm.applyBandPreferenceQrtr() },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    "Apply",
                    color = Color.White,
                    fontWeight = FontWeight.Bold,
                    fontSize = 16.sp,
                )
            }
            Spacer(Modifier.height(12.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                HomeChip("LTE all",  Modifier.weight(1f)) { vm.setAllLte(true) }
                HomeChip("LTE none", Modifier.weight(1f)) { vm.setAllLte(false) }
                HomeChip("NR all",   Modifier.weight(1f)) { vm.setAllNr(true) }
                HomeChip("Reset",    Modifier.weight(1f), enabled = !state.busy) {
                    vm.resetBandPreference()
                }
            }
            Spacer(Modifier.height(14.dp))
            HorizontalDivider(color = GlassColors.DividerSoft)
            Spacer(Modifier.height(10.dp))
            Text(
                "LTE",
                color = GlassColors.TextSecondary,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
            )
            Spacer(Modifier.height(4.dp))
            BandCatalog.LTE.forEach { e ->
                GlassHomeBandRow(
                    label = "B${e.number}",
                    sub = e.freqLabel,
                    checked = state.lteMask.contains(e.number),
                    onCheckedChange = { vm.toggleLte(e.number, it) },
                )
            }
            Spacer(Modifier.height(10.dp))
            Text(
                "NR5G",
                color = GlassColors.TextSecondary,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
            )
            Spacer(Modifier.height(4.dp))
            BandCatalog.NR.forEach { e ->
                GlassHomeBandRow(
                    label = "n${e.number}",
                    sub = e.freqLabel,
                    checked = state.nrMask.contains(e.number),
                    onCheckedChange = { vm.toggleNr(e.number, it) },
                )
            }
        }

        /* ---- Card 3: LTE Cell Table ----------------------------------- */
        val lteSnap = state.snapshots[Rat.LTE] ?: RatSnapshot(Rat.LTE, available = false)
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .glassPanel(cornerDp = 24)
                .padding(horizontal = 12.dp, vertical = 14.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    Icons.Default.Menu,
                    contentDescription = null,
                    tint = GlassColors.TextSecondary,
                    modifier = Modifier.size(18.dp),
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    "LTE Cell Table",
                    color = GlassColors.TextPrimary,
                    fontWeight = FontWeight.Bold,
                    fontSize = 18.sp,
                )
            }
            Spacer(Modifier.height(10.dp))
            CellTable(Rat.LTE, lteSnap.rows)
        }

        Spacer(Modifier.height(4.dp))
    }
}

@Composable
private fun HomeChip(
    text: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Box(
        modifier = modifier
            .height(36.dp)
            .glassPill()
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text,
            color = GlassColors.TextPrimary,
            fontSize = 12.sp,
            fontWeight = FontWeight.Medium,
        )
    }
}

@Composable
private fun GlassHomeBandRow(
    label: String,
    sub: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp),
    ) {
        Checkbox(
            checked = checked,
            onCheckedChange = onCheckedChange,
            colors = CheckboxDefaults.colors(
                checkedColor = GlassColors.Accent,
                uncheckedColor = GlassColors.TextTertiary,
                checkmarkColor = Color.White,
            ),
        )
        Spacer(Modifier.width(4.dp))
        Text(
            label,
            color = GlassColors.TextPrimary,
            fontWeight = FontWeight.Bold,
            fontSize = 14.sp,
            modifier = Modifier.width(48.dp),
        )
        Text(
            sub,
            color = GlassColors.TextSecondary,
            fontSize = 12.sp,
            modifier = Modifier.weight(1f),
        )
    }
}

private fun rootShort(s: RootClient.Status): String = when (s) {
    is RootClient.Status.HasRoot -> "ok"
    is RootClient.Status.NoRoot  -> "no"
    is RootClient.Status.Error   -> "err"
    is RootClient.Status.Unknown -> "?"
}

@Composable
private fun DrawerRow(text: String, color: Color = NsgColors.TextPrimary, onClick: () -> Unit) {
    NavigationDrawerItem(
        label = { Text(text, color = color) },
        selected = false,
        onClick = onClick,
        modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun RatPagerSection(state: UiState, modifier: Modifier = Modifier) {
    val pagerState = rememberPagerState(initialPage = 2 /* LTE */) { RAT_ORDER.size }
    Column(modifier) {
        HorizontalPager(
            state = pagerState,
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
        ) { page ->
            val rat = RAT_ORDER[page]
            val snap = state.snapshots[rat] ?: RatSnapshot(rat, available = false)
            Column(
                Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.Top,
            ) {
                RatPage(snap)
            }
        }
        PagerDots(RAT_ORDER.size, pagerState.currentPage)
    }
}

/* ----------------------- Band Lock panel ----------------------- */

@Composable
private fun BandLockPanel(state: UiState, vm: MainViewModel) {
    val lteCount = state.lteMask.enabledBands().size
    val nrCount  = state.nrMask.enabledBands().size

    Column(Modifier.fillMaxSize()) {
        Text(
            "Band preference",
            color = NsgColors.Accent,
            fontWeight = FontWeight.Bold,
            fontSize = 15.sp,
        )
        Text(
            "LTE: $lteCount selected   •   NR5G: $nrCount selected",
            color = NsgColors.TextLabel,
            fontSize = 11.sp,
            modifier = Modifier.padding(top = 2.dp),
        )

        Spacer(Modifier.height(10.dp))
        Button(
            onClick = vm::applyBandPreferenceQrtr,
            enabled = !state.busy,
            modifier = Modifier.fillMaxWidth().height(44.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = NsgColors.Accent,
                contentColor = Color.White,
            ),
        ) { Text("Apply", fontWeight = FontWeight.Bold, fontSize = 14.sp) }

        Spacer(Modifier.height(6.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            OutlinedButton(
                onClick = { vm.setAllLte(true) },
                modifier = Modifier.weight(1f),
                contentPadding = PaddingValues(horizontal = 4.dp, vertical = 6.dp),
            ) { Text("LTE all", fontSize = 12.sp) }
            OutlinedButton(
                onClick = { vm.setAllLte(false) },
                modifier = Modifier.weight(1f),
                contentPadding = PaddingValues(horizontal = 4.dp, vertical = 6.dp),
            ) { Text("LTE none", fontSize = 12.sp) }
            OutlinedButton(
                onClick = { vm.setAllNr(true) },
                modifier = Modifier.weight(1f),
                contentPadding = PaddingValues(horizontal = 4.dp, vertical = 6.dp),
            ) { Text("NR all", fontSize = 12.sp) }
            OutlinedButton(
                onClick = { vm.setAllNr(false) },
                modifier = Modifier.weight(1f),
                contentPadding = PaddingValues(horizontal = 4.dp, vertical = 6.dp),
            ) { Text("NR none", fontSize = 12.sp) }
            OutlinedButton(
                onClick = vm::resetBandPreference,
                enabled = !state.busy,
                modifier = Modifier.weight(1f),
                contentPadding = PaddingValues(horizontal = 4.dp, vertical = 6.dp),
            ) { Text("Reset", fontSize = 12.sp) }
        }

        Spacer(Modifier.height(12.dp))
        HorizontalDivider(color = NsgColors.Divider)
        Spacer(Modifier.height(8.dp))

        LazyColumn(Modifier.weight(1f, fill = true)) {
            item {
                SectionHeader("LTE")
            }
            items(BandCatalog.LTE) { e ->
                BandCheckboxRow(
                    label = "B${e.number}",
                    sub = e.freqLabel,
                    checked = state.lteMask.contains(e.number),
                    onCheckedChange = { vm.toggleLte(e.number, it) },
                )
            }
            item {
                Spacer(Modifier.height(10.dp))
                SectionHeader("NR5G")
            }
            items(BandCatalog.NR) { e ->
                BandCheckboxRow(
                    label = "n${e.number}",
                    sub = e.freqLabel,
                    checked = state.nrMask.contains(e.number),
                    onCheckedChange = { vm.toggleNr(e.number, it) },
                )
            }
        }
    }
}

@Composable
private fun SectionHeader(text: String) {
    Text(
        text,
        color = NsgColors.TextLabel,
        fontSize = 11.sp,
        fontWeight = FontWeight.Bold,
        modifier = Modifier.padding(vertical = 4.dp),
    )
}

@Composable
private fun BandCheckboxRow(
    label: String,
    sub: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp),
    ) {
        Checkbox(checked = checked, onCheckedChange = onCheckedChange)
        Spacer(Modifier.width(4.dp))
        Text(
            label,
            color = NsgColors.TextPrimary,
            fontWeight = FontWeight.Bold,
            fontSize = 14.sp,
            modifier = Modifier.width(48.dp),
        )
        Text(
            sub,
            color = NsgColors.TextLabel,
            fontSize = 12.sp,
            modifier = Modifier.weight(1f),
        )
    }
}

/* ----------------------- Band Lock panel (Liquid Glass variant) ----------------------- */

@Composable
private fun GlassBandLockPanel(state: UiState, vm: MainViewModel) {
    val lteCount = state.lteMask.enabledBands().size
    val nrCount  = state.nrMask.enabledBands().size

    Column(Modifier.fillMaxSize()) {
        Text(
            "Band preference",
            color = GlassColors.TextPrimary,
            fontWeight = FontWeight.Bold,
            fontSize = 17.sp,
        )
        Text(
            "LTE: $lteCount selected   •   NR5G: $nrCount selected",
            color = GlassColors.TextSecondary,
            fontSize = 12.sp,
            modifier = Modifier.padding(top = 2.dp),
        )

        Spacer(Modifier.height(14.dp))
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(50.dp)
                .glassPrimaryPill()
                .clickable(enabled = !state.busy) { vm.applyBandPreferenceQrtr() },
            contentAlignment = Alignment.Center,
        ) {
            Text(
                "Apply",
                color = Color.White,
                fontWeight = FontWeight.Bold,
                fontSize = 15.sp,
            )
        }

        Spacer(Modifier.height(10.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            GlassPillButton("LTE all",  Modifier.weight(1f)) { vm.setAllLte(true) }
            GlassPillButton("LTE none", Modifier.weight(1f)) { vm.setAllLte(false) }
            GlassPillButton("NR all",   Modifier.weight(1f)) { vm.setAllNr(true) }
            GlassPillButton("NR none",  Modifier.weight(1f)) { vm.setAllNr(false) }
            GlassPillButton("Reset",    Modifier.weight(1f), enabled = !state.busy) { vm.resetBandPreference() }
        }

        Spacer(Modifier.height(14.dp))
        HorizontalDivider(color = GlassColors.DividerSoft)
        Spacer(Modifier.height(8.dp))

        LazyColumn(Modifier.weight(1f, fill = true)) {
            item { GlassSectionHeader("LTE") }
            items(BandCatalog.LTE) { e ->
                GlassBandCheckboxRow(
                    label = "B${e.number}",
                    sub = e.freqLabel,
                    checked = state.lteMask.contains(e.number),
                    onCheckedChange = { vm.toggleLte(e.number, it) },
                )
            }
            item {
                Spacer(Modifier.height(10.dp))
                GlassSectionHeader("NR5G")
            }
            items(BandCatalog.NR) { e ->
                GlassBandCheckboxRow(
                    label = "n${e.number}",
                    sub = e.freqLabel,
                    checked = state.nrMask.contains(e.number),
                    onCheckedChange = { vm.toggleNr(e.number, it) },
                )
            }
        }
    }
}

@Composable
private fun GlassPillButton(
    text: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Box(
        modifier = modifier
            .height(36.dp)
            .glassPill()
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text,
            color = GlassColors.TextPrimary,
            fontSize = 12.sp,
            fontWeight = FontWeight.Medium,
        )
    }
}

@Composable
private fun GlassSectionHeader(text: String) {
    Text(
        text,
        color = GlassColors.TextSecondary,
        fontSize = 12.sp,
        fontWeight = FontWeight.Bold,
        modifier = Modifier.padding(vertical = 4.dp),
    )
}

@Composable
private fun GlassBandCheckboxRow(
    label: String,
    sub: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp),
    ) {
        Checkbox(
            checked = checked,
            onCheckedChange = onCheckedChange,
            colors = CheckboxDefaults.colors(
                checkedColor = GlassColors.Accent,
                uncheckedColor = GlassColors.TextTertiary,
                checkmarkColor = Color.White,
            ),
        )
        Spacer(Modifier.width(4.dp))
        Text(
            label,
            color = GlassColors.TextPrimary,
            fontWeight = FontWeight.Bold,
            fontSize = 14.sp,
            modifier = Modifier.width(48.dp),
        )
        Text(
            sub,
            color = GlassColors.TextSecondary,
            fontSize = 12.sp,
            modifier = Modifier.weight(1f),
        )
    }
}

/* ----------------------- Cell Lock panel ----------------------- */

@Composable
private fun CellLockPanel(state: UiState, vm: MainViewModel) {
    Column(Modifier.fillMaxWidth()) {
        Text(
            "LTE PCI lock",
            color = NsgColors.Accent,
            fontWeight = FontWeight.Bold,
            fontSize = 14.sp,
        )
        Text(
            "⚠ Opcode unverified for Snapdragon X70 (Poco F6). " +
                "If rc ≠ 0, capture frames via DIAG Sniffer and compare with Network Signal Guru.",
            color = NsgColors.TextLabel,
            fontSize = 11.sp,
            modifier = Modifier.padding(vertical = 4.dp),
        )
        Row {
            OutlinedTextField(
                value = state.lockEarfcn,
                onValueChange = { vm.setLockInput(it, state.lockPci) },
                label = { Text("EARFCN") },
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            OutlinedTextField(
                value = state.lockPci,
                onValueChange = { vm.setLockInput(state.lockEarfcn, it) },
                label = { Text("PCI") },
                modifier = Modifier.weight(1f),
            )
        }
        Spacer(Modifier.height(6.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = vm::applyCellLock,
                enabled = !state.busy,
                colors = ButtonDefaults.buttonColors(
                    containerColor = NsgColors.Accent,
                    contentColor = Color.White,
                ),
            ) { Text("Lock", fontWeight = FontWeight.Bold) }
            OutlinedButton(onClick = vm::clearCellLock, enabled = !state.busy) { Text("Unlock") }
        }
    }
}

/* ----------------------- Sniffer panel ----------------------- */

private val monoSmall = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 11.sp)

@Composable
private fun SnifferPanel(state: UiState, vm: MainViewModel) {
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "DIAG sniffer",
                color = NsgColors.Accent,
                fontWeight = FontWeight.Bold,
                fontSize = 14.sp,
                modifier = Modifier.weight(1f),
            )
            Text(
                "${state.snifferFrames.size}/${state.snifferTotal} (drop ${state.snifferDropped})",
                color = NsgColors.TextLabel,
                fontSize = 11.sp,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            val snifferColors = ButtonDefaults.buttonColors(
                containerColor = NsgColors.Accent,
                contentColor = Color.White,
            )
            if (!state.snifferRunning)
                Button(onClick = vm::startSniffer, enabled = !state.busy, colors = snifferColors) {
                    Text("Start", fontWeight = FontWeight.Bold)
                }
            else
                Button(onClick = vm::stopSniffer, enabled = !state.busy, colors = snifferColors) {
                    Text("Stop", fontWeight = FontWeight.Bold)
                }
            OutlinedButton(onClick = vm::saveSnifferToFile, enabled = !state.busy) { Text("Save") }
            OutlinedButton(onClick = vm::clearSnifferUi) { Text("Clear") }
        }
        state.snifferSavePath?.let {
            Text(it, color = NsgColors.TextLabel, fontSize = 11.sp, modifier = Modifier.padding(top = 4.dp))
        }
        Spacer(Modifier.height(6.dp))
        HorizontalDivider(color = NsgColors.Divider)
        if (state.snifferFrames.isEmpty()) {
            Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) {
                Text(
                    if (state.snifferRunning) "Waiting for DIAG frames…" else "Sniffer idle.",
                    color = NsgColors.TextDim,
                )
            }
        } else {
            LazyColumn(Modifier.fillMaxWidth().heightIn(max = 260.dp)) {
                items(state.snifferFrames.asReversed()) { f ->
                    Column(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                        Row {
                            Text(
                                "#${f.seq}",
                                color = NsgColors.TextLabel,
                                style = monoSmall,
                                modifier = Modifier.width(48.dp),
                            )
                            Text(
                                f.subsysLabel,
                                color = NsgColors.ChannelHi,
                                style = monoSmall,
                                modifier = Modifier.width(120.dp),
                            )
                            Text(
                                "${f.bytes.size} B",
                                color = NsgColors.TextLabel,
                                style = monoSmall,
                            )
                        }
                        Text(
                            f.hexPreview(32),
                            color = NsgColors.TextPrimary,
                            style = monoSmall,
                        )
                    }
                }
            }
        }
    }
}

/* ----------------------- Log panel ----------------------- */

@Composable
private fun LogPanel(state: UiState, vm: MainViewModel) {
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "Command log",
                color = NsgColors.Accent,
                fontWeight = FontWeight.Bold,
                fontSize = 14.sp,
                modifier = Modifier.weight(1f),
            )
            val bound = state.apiBound
            Button(
                onClick = vm::bindRootService,
                colors = ButtonDefaults.buttonColors(
                    containerColor = NsgColors.Accent,
                    contentColor = Color.White,
                ),
            ) {
                Text(if (bound) "Reconnect root" else "Connect root", fontWeight = FontWeight.Bold)
            }
            Spacer(Modifier.width(6.dp))
            OutlinedButton(onClick = vm::openDiag) { Text("Open /dev/diag") }
        }
        Spacer(Modifier.height(4.dp))
        Box(
            Modifier
                .fillMaxWidth()
                .heightIn(max = 280.dp)
                .background(NsgColors.Background)
                .padding(8.dp)
                .verticalScroll(rememberScrollState()),
        ) {
            Text(
                text = state.log.ifEmpty { "(no commands yet)" },
                color = NsgColors.TextPrimary,
                style = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 11.sp),
            )
        }
    }
}

/* ----------------------- NV Inspector panel ----------------------- */

@Composable
private fun NvInspectorPanel(state: UiState, vm: MainViewModel) {
    Column(Modifier.fillMaxWidth()) {
        Text(
            "NV Inspector (EFS2)",
            color = NsgColors.Accent,
            fontWeight = FontWeight.Bold,
            fontSize = 14.sp,
        )
        Text(
            "Read/Write/Delete Qualcomm NV-items over DIAG EFS2. " +
                "Same mechanism as NSG and Qct Modem Capabilities.",
            color = NsgColors.TextLabel,
            fontSize = 10.sp,
            modifier = Modifier.padding(vertical = 2.dp),
        )
        Spacer(Modifier.height(4.dp))
        LazyColumn(Modifier.fillMaxWidth().heightIn(max = 110.dp)) {
            items(NvPresets.ALL) { (label, path) ->
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 1.dp),
                ) {
                    OutlinedButton(
                        onClick = { vm.setNvPath(path) },
                        modifier = Modifier.width(84.dp),
                    ) { Text("Pick", fontSize = 11.sp) }
                    Spacer(Modifier.width(6.dp))
                    Column {
                        Text(label, color = NsgColors.TextPrimary, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                        Text(path, color = NsgColors.TextDim, style = monoSmall)
                    }
                }
            }
        }
        Spacer(Modifier.height(4.dp))
        OutlinedTextField(
            value = state.nvPath,
            onValueChange = vm::setNvPath,
            label = { Text("NV path") },
            modifier = Modifier.fillMaxWidth(),
            textStyle = monoSmall,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Button(
                onClick = vm::readNv,
                enabled = !state.busy,
                colors = ButtonDefaults.buttonColors(
                    containerColor = NsgColors.Accent,
                    contentColor = Color.White,
                ),
            ) { Text("Read", fontWeight = FontWeight.Bold) }
            OutlinedButton(onClick = vm::deleteNv, enabled = !state.busy) { Text("Unlink") }
            OutlinedButton(onClick = vm::writeNv, enabled = !state.busy) { Text("Write hex") }
        }
        OutlinedTextField(
            value = state.nvWriteHex,
            onValueChange = vm::setNvWriteHex,
            label = { Text("Write payload (hex, e.g. 01 02 FF)") },
            modifier = Modifier.fillMaxWidth(),
            textStyle = monoSmall,
        )
        Spacer(Modifier.height(4.dp))
        Box(
            Modifier
                .fillMaxWidth()
                .heightIn(max = 140.dp)
                .background(NsgColors.Background)
                .padding(6.dp)
                .verticalScroll(rememberScrollState()),
        ) {
            Text(
                text = state.nvHex.ifEmpty { "(no NV item read yet)" },
                color = NsgColors.TextPrimary,
                style = monoSmall,
            )
        }
    }
}
