<template>
    <div class="card p-2 mt-4">
        <div class="card-body">
            <h2>{{ $t('resource_card.resources') }}</h2>
            <br>
            <p>{{ $t('resource_card.resources_desc') }}</p>
            <div class="card-group p-4 align-items-center">
                <a class="btn btn-secondary m-1" href="https://github.com/r0mn-creator/stand-in-server" target="_blank">
                    <i class="fab fa-fw fa-github"></i> GitHub</a>
            </div>
        </div>
    </div>
    <!-- Legal -->
    <div class="card p-2 mt-4">
        <div class="card-body">
            <h2>{{ $t('resource_card.legal') }}</h2>
            <br>
            <p>{{ $t('resource_card.legal_desc') }}</p>
            <div class="card-group p-4 align-items-center">
                <a class="btn btn-danger m-1" href="https://github.com/r0mn-creator/stand-in-server/blob/main/LICENSE"
                    target="_blank">
                    <i class="fas fa-fw fa-file-alt"></i> {{ $t('resource_card.license') }}</a>
                <a class="btn btn-danger m-1" href="https://github.com/r0mn-creator/stand-in-server/blob/main/NOTICE"
                    target="_blank">
                    <i class="fas fa-fw fa-exclamation"></i> {{ $t('resource_card.third_party_notice') }}</a>
            </div>
            <p class="mb-0 text-body-secondary">Based on Apollo and Sunshine (GPLv3)</p>
        </div>
    </div>
</template>
