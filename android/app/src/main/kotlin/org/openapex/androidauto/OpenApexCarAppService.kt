package org.openapex.androidauto

import android.content.Intent
import androidx.car.app.CarAppService
import androidx.car.app.Session
import androidx.car.app.SessionInfo
import androidx.car.app.Screen
import androidx.car.app.model.Pane
import androidx.car.app.model.PaneTemplate
import androidx.car.app.model.Row
import androidx.car.app.model.Template
import androidx.car.app.validation.HostValidator

class OpenApexCarAppService : CarAppService() {
    override fun createHostValidator(): HostValidator = HostValidator.ALLOW_ALL_HOSTS_VALIDATOR

    override fun onCreateSession(sessionInfo: SessionInfo): Session = OpenApexSession()
}

private class OpenApexSession : Session() {
    override fun onCreateScreen(intent: Intent): Screen = OpenApexScreen(carContext)
}

private class OpenApexScreen(
    carContext: androidx.car.app.CarContext,
) : Screen(carContext) {
    override fun onGetTemplate(): Template = PaneTemplate.Builder(
        Pane.Builder()
            .addRow(
                Row.Builder()
                    .setTitle("Terminal companion")
                    .addText("Phase 1 notification and GNSS passthrough")
                    .build(),
            )
            .build(),
    )
        .setTitle("OpenApex")
        .build()
}
