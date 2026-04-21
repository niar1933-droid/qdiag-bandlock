package com.qdiag.bandlock.ui

import androidx.compose.foundation.background
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material3.Checkbox
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
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
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

private enum class DrawerPanel { None, BandLock, CellLock, Sniffer, Log }

private val RAT_ORDER = listOf(Rat.GSM, Rat.WCDMA, Rat.LTE, Rat.NR)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(vm: MainViewModel) {
    val state by vm.ui.collectAsStateWithLifecycle()
    val drawerState = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    var activePanel by remember { mutableStateOf(DrawerPanel.None) }

    ModalNavigationDrawer(
        drawerState = drawerState,
        drawerContent = {
            ModalDrawerSheet(
                drawerContainerColor = NsgColors.Surface,
            ) {
                Text(
                    "QDiag Band Lock",
                    color = NsgColors.Accent,
                    fontWeight = FontWeight.Bold,
                    fontSize = 18.sp,
                    modifier = Modifier.padding(16.dp),
                )
                HorizontalDivider(color = NsgColors.Divider)
                DrawerRow("Band Lock (LTE / NR)") {
                    activePanel = DrawerPanel.BandLock
                    scope.launch { drawerState.close() }
                }
                DrawerRow("Cell Lock (LTE PCI)") {
                    activePanel = DrawerPanel.CellLock
                    scope.launch { drawerState.close() }
                }
                DrawerRow("DIAG Sniffer") {
                    activePanel = DrawerPanel.Sniffer
                    scope.launch { drawerState.close() }
                }
                DrawerRow("Command Log") {
                    activePanel = DrawerPanel.Log
                    scope.launch { drawerState.close() }
                }
                HorizontalDivider(color = NsgColors.Divider)
                DrawerRow("Close panel") {
                    activePanel = DrawerPanel.None
                    scope.launch { drawerState.close() }
                }
            }
        },
    ) {
        Scaffold(
            containerColor = NsgColors.Background,
            topBar = {
                TopAppBar(
                    title = { Text("QDiag", color = NsgColors.Accent, fontWeight = FontWeight.Bold) },
                    navigationIcon = {
                        IconButton(onClick = { scope.launch { drawerState.open() } }) {
                            Icon(Icons.Default.Menu, "menu", tint = NsgColors.TextPrimary)
                        }
                    },
                    actions = {
                        Text(
                            text = "root:${rootShort(state.rootStatus)}  " +
                                "svc:${if (state.apiBound) "ok" else "-"}  " +
                                "diag:${if (state.diagOpen) "open" else "-"}",
                            color = NsgColors.TextLabel,
                            fontSize = 12.sp,
                            modifier = Modifier.padding(end = 12.dp),
                        )
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = NsgColors.Surface,
                        titleContentColor = NsgColors.Accent,
                    ),
                )
            },
        ) { padding ->
            Column(
                Modifier
                    .padding(padding)
                    .fillMaxSize()
                    .background(NsgColors.Background),
            ) {
                RatPagerSection(state, Modifier.weight(1f))
                if (activePanel != DrawerPanel.None) {
                    HorizontalDivider(color = NsgColors.Divider)
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(max = 360.dp)
                            .background(NsgColors.Surface)
                            .padding(12.dp),
                    ) {
                        when (activePanel) {
                            DrawerPanel.BandLock -> BandLockPanel(state, vm)
                            DrawerPanel.CellLock -> CellLockPanel(state, vm)
                            DrawerPanel.Sniffer  -> SnifferPanel(state, vm)
                            DrawerPanel.Log      -> LogPanel(state, vm)
                            DrawerPanel.None     -> {}
                        }
                    }
                }
            }
        }
    }
}

private fun rootShort(s: RootClient.Status): String = when (s) {
    is RootClient.Status.HasRoot -> "ok"
    is RootClient.Status.NoRoot  -> "no"
    is RootClient.Status.Error   -> "err"
    is RootClient.Status.Unknown -> "?"
}

@Composable
private fun DrawerRow(text: String, onClick: () -> Unit) {
    NavigationDrawerItem(
        label = { Text(text, color = NsgColors.TextPrimary) },
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
            Column(Modifier.verticalScroll(rememberScrollState())) {
                RatPage(snap)
            }
        }
        PagerDots(RAT_ORDER.size, pagerState.currentPage)
    }
}

/* ----------------------- Band Lock panel ----------------------- */

@Composable
private fun BandLockPanel(state: UiState, vm: MainViewModel) {
    Column(Modifier.fillMaxWidth()) {
        Text(
            "Band preference",
            color = NsgColors.Accent,
            fontWeight = FontWeight.Bold,
            fontSize = 14.sp,
        )
        Spacer(Modifier.height(6.dp))
        Row {
            OutlinedButton(onClick = { vm.setAllLte(true) }) { Text("LTE all") }
            Spacer(Modifier.width(6.dp))
            OutlinedButton(onClick = { vm.setAllLte(false) }) { Text("LTE none") }
            Spacer(Modifier.width(12.dp))
            OutlinedButton(onClick = { vm.setAllNr(true) }) { Text("NR all") }
            Spacer(Modifier.width(6.dp))
            OutlinedButton(onClick = { vm.setAllNr(false) }) { Text("NR none") }
        }
        Spacer(Modifier.height(4.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilledTonalButton(onClick = vm::applyBandPreference) { Text("Apply") }
            OutlinedButton(onClick = vm::resetBandPreference) { Text("Reset to ALL") }
        }
        Spacer(Modifier.height(8.dp))
        LazyColumn {
            item {
                Text("LTE", color = NsgColors.TextLabel, fontSize = 12.sp, fontWeight = FontWeight.Bold)
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
                Spacer(Modifier.height(6.dp))
                Text("NR5G", color = NsgColors.TextLabel, fontSize = 12.sp, fontWeight = FontWeight.Bold)
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
        Spacer(Modifier.width(6.dp))
        Text(label, color = NsgColors.TextPrimary, fontWeight = FontWeight.Bold, modifier = Modifier.width(60.dp))
        Text(sub, color = NsgColors.TextLabel, fontSize = 12.sp)
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
            FilledTonalButton(onClick = vm::applyCellLock) { Text("Lock") }
            OutlinedButton(onClick = vm::clearCellLock) { Text("Unlock") }
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
            if (!state.snifferRunning)
                FilledTonalButton(onClick = vm::startSniffer) { Text("Start") }
            else
                FilledTonalButton(onClick = vm::stopSniffer) { Text("Stop") }
            OutlinedButton(onClick = vm::saveSnifferToFile) { Text("Save") }
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
            FilledTonalButton(onClick = vm::bindRootService) {
                Text(if (bound) "Reconnect root" else "Connect root")
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
