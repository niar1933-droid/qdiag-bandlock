package com.qdiag.bandlock.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.qdiag.bandlock.qmi.BandCatalog
import com.qdiag.bandlock.root.RootClient
import com.qdiag.bandlock.telephony.CellSnapshot

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(vm: MainViewModel) {
    val state by vm.ui.collectAsStateWithLifecycle()
    Scaffold(topBar = { TopAppBar(title = { Text("QDiag Band Lock") }) }) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .padding(12.dp)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            RootCard(state, vm)
            CurrentCellCard(state.cells, onRefresh = vm::refreshCells)
            BandPrefCard(state, vm)
            CellLockCard(state, vm)
            LogCard(state.log)
        }
    }
}

@Composable
private fun RootCard(state: UiState, vm: MainViewModel) {
    Card {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text("Root & DIAG", style = androidx.compose.material3.MaterialTheme.typography.titleMedium)
            Text("Root: " + when (state.rootStatus) {
                is RootClient.Status.HasRoot -> "granted"
                is RootClient.Status.NoRoot  -> "denied"
                is RootClient.Status.Error   -> "error: ${state.rootStatus.message}"
                is RootClient.Status.Unknown -> "checking…"
            })
            Text("/dev/diag: " + if (state.diagOpen) "open" else "closed")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = vm::bindRootService) { Text("Connect root service") }
                OutlinedButton(onClick = vm::openDiag) { Text("Open /dev/diag") }
                OutlinedButton(onClick = vm::closeDiag) { Text("Close") }
            }
        }
    }
}

@Composable
private fun CurrentCellCard(cells: List<CellSnapshot>, onRefresh: () -> Unit) {
    Card {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Current cells", style = androidx.compose.material3.MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                OutlinedButton(onClick = onRefresh) { Text("Refresh") }
            }
            if (cells.isEmpty()) {
                Text("No cell info. Grant location permission and ensure a SIM is active.")
            } else {
                cells.forEach { c ->
                    Column {
                        Text(
                            text = "${c.rat}  PCI=${c.pci ?: "?"}  EARFCN/ARFCN=${c.earfcnOrArfcn ?: "?"}" +
                                (c.band?.let { "  B$it" } ?: "") +
                                (if (c.registered) "  [serving]" else ""),
                            fontWeight = FontWeight.SemiBold,
                        )
                        Text("CellId=${c.cellId ?: "?"}  TAC=${c.tac ?: "?"}  MCC/MNC=${c.mcc}/${c.mnc}  RSRP=${c.rsrpDbm ?: "?"} dBm  RSRQ=${c.rsrq ?: "?"}")
                        HorizontalDivider(Modifier.padding(vertical = 4.dp))
                    }
                }
            }
        }
    }
}

@Composable
private fun BandPrefCard(state: UiState, vm: MainViewModel) {
    Card {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text("Band preference", style = androidx.compose.material3.MaterialTheme.typography.titleMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { vm.setAllLte(true) }) { Text("All LTE") }
                OutlinedButton(onClick = { vm.setAllLte(false) }) { Text("No LTE") }
                OutlinedButton(onClick = { vm.setAllNr(true) }) { Text("All NR") }
                OutlinedButton(onClick = { vm.setAllNr(false) }) { Text("No NR") }
            }
            Text("LTE", fontWeight = FontWeight.SemiBold)
            BandGrid(BandCatalog.LTE, { state.lteMask.contains(it) }, vm::toggleLte)
            Text("NR5G", fontWeight = FontWeight.SemiBold)
            BandGrid(BandCatalog.NR, { state.nrMask.contains(it) }, vm::toggleNr)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = vm::applyBandPreference, enabled = !state.busy) { Text("Apply") }
                OutlinedButton(onClick = vm::resetBandPreference, enabled = !state.busy) { Text("Reset (all bands)") }
                if (state.busy) CircularProgressIndicator(Modifier.padding(start = 8.dp))
            }
        }
    }
}

@Composable
private fun BandGrid(
    entries: List<BandCatalog.Entry>,
    isEnabled: (Int) -> Boolean,
    onToggle: (Int, Boolean) -> Unit,
) {
    /* Simple 2-per-row layout keeps deps minimal. */
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        entries.chunked(2).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { entry ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.weight(1f),
                    ) {
                        Checkbox(
                            checked = isEnabled(entry.number),
                            onCheckedChange = { onToggle(entry.number, it) },
                        )
                        Text("B${entry.number}  ${entry.freqLabel}")
                    }
                }
                if (row.size == 1) Spacer(Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun CellLockCard(state: UiState, vm: MainViewModel) {
    Card {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text("LTE cell lock (PCI + EARFCN)", style = androidx.compose.material3.MaterialTheme.typography.titleMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = state.lockEarfcn,
                    onValueChange = { vm.setLockInput(it.filter { c -> c.isDigit() }, state.lockPci) },
                    label = { Text("EARFCN") },
                    singleLine = true,
                    modifier = Modifier.weight(1f),
                )
                OutlinedTextField(
                    value = state.lockPci,
                    onValueChange = { vm.setLockInput(state.lockEarfcn, it.filter { c -> c.isDigit() }) },
                    label = { Text("PCI") },
                    singleLine = true,
                    modifier = Modifier.weight(1f),
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = vm::applyCellLock, enabled = !state.busy) { Text("Lock cell") }
                OutlinedButton(onClick = vm::clearCellLock, enabled = !state.busy) { Text("Unlock") }
            }
            Text(
                text = "Per-baseband caveat: the opcode set in qmi_nas.c is known to work on SDX5x/SDX65 modems. " +
                    "Older MDM9x07 / Snapdragon 8xx/7xx baseband builds may need the opcode patched in the native module.",
                style = androidx.compose.material3.MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun LogCard(log: String) {
    Card(colors = CardDefaults.cardColors()) {
        Column(Modifier.padding(12.dp)) {
            Text("Log", style = androidx.compose.material3.MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(4.dp))
            Surface(
                shape = RoundedCornerShape(6.dp),
                tonalElevation = 2.dp,
                modifier = Modifier.fillMaxWidth().heightIn(min = 120.dp, max = 280.dp),
            ) {
                LazyColumn(Modifier.padding(8.dp)) {
                    items(log.lines()) { line ->
                        Text(line, fontFamily = FontFamily.Monospace,
                             style = androidx.compose.material3.MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
    }
}
