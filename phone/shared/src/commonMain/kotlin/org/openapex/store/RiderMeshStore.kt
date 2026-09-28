package org.openapex.store

import org.openapex.protocol.Friend
import org.openapex.protocol.RiderIdentity

/**
 * The slice of the rider store the mesh sync needs. Narrower than [RiderStoreCore] on purpose: the
 * sync neither renames the rider nor removes friends, and a platform whose store is guarded by a
 * lock can hand over an adapter that holds it rather than the core itself.
 */
interface RiderMeshStore {
    fun identity(): RiderIdentity
    fun friends(): List<Friend>
    fun setFriends(list: List<Friend>)
}
